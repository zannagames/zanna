//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the lightweight parser that converts textual runtime signature
// specifications into structured @ref RuntimeSignature objects.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Parsing helpers for runtime signature specifications.
/// @details Translates the textual signature format emitted by the runtime data
///          generator into structured return/parameter type lists for use by the
///          runtime bridge.

#include "il/runtime/RuntimeSignatureParser.hpp"

#include <cctype>
#include <optional>

namespace il::runtime {
namespace {

/// @brief Determine whether a character is considered whitespace by the parser.
/// @param ch Character to inspect.
/// @return True when @p ch is a space, tab, carriage return, or newline.
constexpr bool isWhitespace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

/// @brief Map a textual token to an @ref il::core::Type::Kind.
/// @details Accepts the mnemonics emitted by the runtime generator and performs
///          a case-insensitive match for select aliases (e.g. `bool`).
/// @param token Token to translate.
/// @return Parsed kind, or empty when unknown.
std::optional<il::core::Type::Kind> parseKindToken(std::string_view token) {
    using Kind = il::core::Type::Kind;
    token = trim(token);
    // Optional types (trailing '?') map to Ptr at the IL level (nullable pointer)
    if (!token.empty() && token.back() == '?')
        return Kind::Ptr;
    if (token == "void")
        return Kind::Void;
    if (token == "i1" || token == "bool")
        return Kind::I1;
    if (token == "i16")
        return Kind::I16;
    if (token == "i32")
        return Kind::I32;
    if (token == "i64")
        return Kind::I64;
    if (token == "f64")
        return Kind::F64;
    if (token == "str" || token == "string")
        return Kind::Str;
    if (token.rfind("ptr", 0) == 0 || token == "obj" || token.rfind("obj<", 0) == 0)
        return Kind::Ptr;
    // Parameterized seq/list types (e.g. "seq<str>", "list<i64>") are opaque
    // pointers at the IL level. The element type is used only by Zia-layer sema
    // (ParsedSignature.elementTypeName) for producing typed sequence types.
    if (token == "seq" || token.rfind("seq<", 0) == 0 || token == "list" ||
        token.rfind("list<", 0) == 0)
        return Kind::Ptr;
    if (token == "resume" || token == "resume_tok")
        return Kind::ResumeTok;
    return std::nullopt;
}

/// @brief Determine whether a parameter token denotes a managed object handle.
/// @details Nullable suffixes are ignored. Object, sequence, and list spellings
///          all require the safe frontend's managed-object treatment even though
///          their lowered IL type is pointer.
/// @param token Raw parameter type token.
/// @return True when the token contributes a bit to the object-parameter mask.
bool isObjectParamToken(std::string_view token) {
    token = trim(token);
    if (!token.empty() && token.back() == '?')
        token.remove_suffix(1);
    // seq/list parameters are managed runtime sequence objects, not raw
    // pointers, so they get the same safe-frontend treatment as obj.
    return token == "obj" || token.rfind("obj<", 0) == 0 || token == "seq" ||
           token.rfind("seq<", 0) == 0 || token == "list" || token.rfind("list<", 0) == 0;
}

} // namespace

/// @brief Strip leading and trailing ASCII whitespace from a string view.
/// @param text View to trim.
/// @return View referencing the trimmed range of @p text.
std::string_view trim(std::string_view text) {
    while (!text.empty() && isWhitespace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && isWhitespace(text.back()))
        text.remove_suffix(1);
    return text;
}

/// @brief Split a comma-delimited parameter list while respecting parentheses.
/// @details Handles nested parentheses so pointer types are not split apart and
///          removes empty tokens created by redundant commas.
/// @param text Slice containing the comma-separated list between parentheses.
/// @return Sequence of trimmed tokens.
std::vector<std::string_view> splitTypeList(std::string_view text) {
    std::vector<std::string_view> tokens;
    std::size_t start = 0;
    int depth = 0;
    int angleDepth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '(') {
            ++depth;
        } else if (ch == ')') {
            if (depth > 0)
                --depth;
        } else if (ch == '<') {
            ++angleDepth;
        } else if (ch == '>') {
            if (angleDepth > 0)
                --angleDepth;
        } else if (ch == ',' && depth == 0 && angleDepth == 0) {
            tokens.push_back(trim(text.substr(start, i - start)));
            start = i + 1;
        }
    }

    if (start < text.size()) {
        tokens.push_back(trim(text.substr(start)));
    } else if (!text.empty()) {
        tokens.emplace_back();
    }

    std::vector<std::string_view> filtered;
    filtered.reserve(tokens.size());
    for (auto token : tokens) {
        if (!token.empty())
            filtered.push_back(token);
    }
    return filtered;
}

/// @brief Parse a runtime signature specification string.
/// @details Extracts the return type token, parses parameter tokens using
///          @ref splitTypeList, and populates a @ref RuntimeSignature.  Invalid
///          layouts yield a default-constructed signature with empty parameter
///          lists.
/// @param spec Textual specification such as "i32(i32,i32)".
/// @return Structured signature describing the runtime call ABI.
RuntimeSignature parseSignatureSpec(std::string_view spec) {
    RuntimeSignature signature;
    spec = trim(spec);
    const auto open = spec.find('(');
    const auto close = spec.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close <= open ||
        !trim(spec.substr(close + 1)).empty()) {
        signature.valid = false;
        return signature;
    }

    const auto retToken = trim(spec.substr(0, open));
    const auto retKind = parseKindToken(retToken);
    if (!retKind) {
        signature.valid = false;
        return signature;
    }
    signature.retType = il::core::Type(*retKind);

    const auto paramsSlice = spec.substr(open + 1, close - open - 1);
    if (!paramsSlice.empty()) {
        const auto tokens = splitTypeList(paramsSlice);
        signature.paramTypes.reserve(tokens.size());
        for (std::size_t index = 0; index < tokens.size(); ++index) {
            auto token = tokens[index];
            const auto kind = parseKindToken(token);
            if (!kind) {
                signature.valid = false;
                signature.paramTypes.clear();
                signature.objectParamMask = 0;
                return signature;
            }
            signature.paramTypes.emplace_back(*kind);
            if (index < 64 && isObjectParamToken(token))
                signature.objectParamMask |= std::uint64_t{1} << index;
        }
    }

    return signature;
}

} // namespace il::runtime
