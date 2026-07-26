//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/io/FunctionParser_Prototype.cpp
// Purpose: Implementation of function prototype parsing. Handles the "func @name(...) -> type {"
//          syntax including parameter parsing, calling convention, and attributes.
// Key invariants: Creates new function entries in the module with proper temp ID setup.
// Ownership/Lifetime: Populates functions directly within the supplied module.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements transactional parsing of IL function declarations.
 *
 * @details The parser decodes linkage, symbol names, parameters, variadic
 *          markers, return types, calling conventions, attributes, and optional
 *          source metadata. It validates declaration collisions and resource
 *          limits before appending the function and initializing its SSA-name
 *          state; a snapshot restores all affected parser/module state on any
 *          failure.
 */

#include "il/internal/io/FunctionParser.hpp"
#include "il/internal/io/FunctionParser_Internal.hpp"
#include "il/internal/io/TypeParser.hpp"

#include "il/core/Linkage.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace il::io::detail {

namespace {

/// @brief Parse one function parameter and validate its public/internal type policy.
/// @details Accepts both `type %name` and `%name: type` spellings.
/// @param rawParam Raw comma-delimited parameter text.
/// @param lineNo Source line used for diagnostics.
/// @param allowInternalTypes True when `error` and `resumetok` are valid in this prototype.
/// @return Parsed named parameter or a structured syntax/type diagnostic.
Expected<Param> parseParameterToken(const std::string &rawParam,
                                    unsigned lineNo,
                                    bool allowInternalTypes) {
    std::string trimmed = trim(rawParam);
    if (trimmed.empty()) {
        std::ostringstream oss;
        oss << "malformed parameter";
        if (!rawParam.empty())
            oss << " '" << rawParam << "'";
        else
            oss << " ''";
        oss << " (empty entry)";
        return lineError<Param>(lineNo, oss.str());
    }

    std::string ty;
    std::string nm;
    size_t colon = trimmed.find(':');
    if (colon != std::string::npos) {
        std::string left = trim(trimmed.substr(0, colon));
        std::string right = trim(trimmed.substr(colon + 1));
        if (left.empty() || right.empty())
            return lineError<Param>(lineNo, "malformed parameter");
        nm = std::move(left);
        ty = std::move(right);
    } else {
        std::stringstream ps(trimmed);
        ps >> ty >> nm;
        std::string trailing;
        if (ps >> trailing)
            return lineError<Param>(lineNo, "unexpected tokens after parameter name");
    }

    if (ty.empty() || nm.empty())
        return lineError<Param>(lineNo, "malformed parameter");
    if (nm[0] != '%')
        return lineError<Param>(lineNo, "parameter name must start with '%'");
    if (nm.size() == 1)
        return lineError<Param>(lineNo, "missing parameter name");
    if (!isValidILIdentifier(std::string_view(nm).substr(1)))
        return lineError<Param>(lineNo, "malformed parameter name");

    bool ok = true;
    Type parsedTy = parseType(ty, &ok);
    if (!ok || parsedTy.kind == Type::Kind::Void ||
        (!allowInternalTypes &&
         (parsedTy.kind == Type::Kind::Error || parsedTy.kind == Type::Kind::ResumeTok)))
        return lineError<Param>(lineNo, "unknown param type");

    return Param{nm.substr(1), parsedTy};
}

/// @brief Parse the function symbol name from "@name(" syntax.
/// @param cur Header cursor advanced to the opening parenthesis.
/// @return Valid identifier without `@`, or a structured header diagnostic.
Expected<std::string> parseSymbolName(Cursor &cur) {
    cur.skipWs();
    if (cur.atEnd())
        return Expected<std::string>{
            makeSyntaxError(cursorPos(cur), "unexpected end of header", {})};

    const std::size_t searchStart = cur.offset();
    size_t at = cur.view().find('@', searchStart);
    if (at == std::string_view::npos)
        return Expected<std::string>{
            makeSyntaxError(cursorPos(cur), "malformed function header", {})};
    size_t lp = cur.view().find('(', at);
    if (lp == std::string_view::npos)
        return Expected<std::string>{
            makeSyntaxError(cursorPos(cur), "malformed function header", {})};
    std::string name = trim(std::string(cur.view().substr(at + 1, lp - at - 1)));
    if (name.empty())
        return Expected<std::string>{
            makeSyntaxError(cursorPos(cur), "malformed function header", {})};
    if (!isValidILIdentifier(name))
        return Expected<std::string>{
            makeSyntaxError(cursorPos(cur), "malformed function name", {})};
    cur.seek(lp);
    return name;
}

/// @brief Parse the function prototype: "(params) -> rettype".
/// @param cur Cursor positioned after the function name.
/// @param isImport When true, the prototype has no opening brace (import declaration).
/// @return Return type, parameters, vararg state, and trailing calling-convention segment.
Expected<PrototypeParseResult> parsePrototype(Cursor &cur, bool isImport = false) {
    cur.skipWs();
    if (cur.atEnd())
        return Expected<PrototypeParseResult>{
            makeSyntaxError(cursorPos(cur), "unexpected end of header", {})};
    if (!cur.consume('('))
        return Expected<PrototypeParseResult>{
            makeSyntaxError(cursorPos(cur), "malformed function header", {})};
    size_t paramsBegin = cur.offset();
    size_t rp = cur.view().find(')', paramsBegin);
    if (rp == std::string_view::npos)
        return Expected<PrototypeParseResult>{
            makeSyntaxError(cursorPos(cur), "malformed function header", {})};
    std::string paramsStr(cur.view().substr(paramsBegin, rp - paramsBegin));
    cur.seek(rp + 1);

    std::vector<Param> params;
    bool isVarArg = false;
    if (!paramsStr.empty()) {
        std::stringstream pss(paramsStr);
        std::string piece;
        std::vector<std::string> rawPieces;
        while (std::getline(pss, piece, ',')) {
            rawPieces.push_back(piece);
        }
        for (std::size_t i = 0; i < rawPieces.size(); ++i) {
            const std::string trimmed = trim(rawPieces[i]);
            if (trimmed == "...") {
                if (isVarArg || i + 1 != rawPieces.size()) {
                    return Expected<PrototypeParseResult>{
                        makeSyntaxError(cursorPos(cur), "variadic marker must appear last", {})};
                }
                isVarArg = true;
                continue;
            }
            if (isVarArg) {
                return Expected<PrototypeParseResult>{
                    makeSyntaxError(cursorPos(cur), "variadic marker must appear last", {})};
            }
            auto param = parseParameterToken(rawPieces[i], cur.line(), !isImport);
            if (!param)
                return Expected<PrototypeParseResult>{param.error()};
            params.push_back(std::move(param.value()));
        }
    }

    size_t gapStart = cur.offset();
    size_t arrow = cur.view().find("->", gapStart);
    if (arrow == std::string_view::npos) {
        if (trimView(cur.view().substr(gapStart)).empty())
            return Expected<PrototypeParseResult>{
                makeSyntaxError(cursorPos(cur), "unexpected end of header", {})};
        return Expected<PrototypeParseResult>{
            makeSyntaxError(cursorPos(cur), "malformed function header", {})};
    }
    std::string_view ccSegment = cur.view().substr(gapStart, arrow - gapStart);
    cur.seek(arrow + 2);
    cur.skipWs();
    if (cur.atEnd())
        return Expected<PrototypeParseResult>{
            makeSyntaxError(cursorPos(cur), "unexpected end of header", {})};

    if (isImport) {
        // Import declarations have no body. The return type ends before optional
        // attributes or a trailing declaration comment.
        const std::size_t suffixBegin = cur.offset();
        std::string_view suffix = cur.view().substr(suffixBegin);
        const std::size_t hashComment = suffix.find('#');
        const std::size_t slashComment = suffix.find("//");
        const std::size_t semicolonComment = suffix.find(';');
        std::size_t comment = std::string::npos;
        for (std::size_t candidate : {hashComment, slashComment, semicolonComment}) {
            if (candidate != std::string::npos &&
                (candidate == 0 ||
                 std::isspace(static_cast<unsigned char>(suffix[candidate - 1])))) {
                comment = std::min(comment, candidate);
            }
        }
        const std::size_t contentEnd = comment == std::string::npos ? suffix.size() : comment;
        const std::size_t attrStart = suffix.substr(0, contentEnd).find('[');
        const std::size_t retEnd = attrStart == std::string::npos ? contentEnd : attrStart;
        std::string retRaw(trimView(suffix.substr(0, retEnd)));
        bool retOk = true;
        Type retTy = parseType(retRaw, &retOk);
        if (!retOk)
            return Expected<PrototypeParseResult>{
                makeSyntaxError(cursorPos(cur), "unknown return type", {})};
        cur.seek(suffixBegin + retEnd);
        return PrototypeParseResult{Prototype{retTy, std::move(params), isVarArg}, ccSegment};
    }

    size_t brace = cur.view().find('{', cur.offset());
    if (brace == std::string_view::npos) {
        if (trimView(cur.view().substr(cur.offset())).empty())
            return Expected<PrototypeParseResult>{
                makeSyntaxError(cursorPos(cur), "unexpected end of header", {})};
        return Expected<PrototypeParseResult>{
            makeSyntaxError(cursorPos(cur), "malformed function header", {})};
    }
    size_t retEnd = brace;
    size_t attrStart = cur.view().find('[', cur.offset());
    if (attrStart != std::string_view::npos && attrStart < brace)
        retEnd = attrStart;
    std::string retRaw(cur.view().substr(cur.offset(), retEnd - cur.offset()));
    std::string retStr = trim(retRaw);
    bool retOk = true;
    Type retTy = parseType(retStr, &retOk);
    if (!retOk)
        return Expected<PrototypeParseResult>{
            makeSyntaxError(cursorPos(cur), "unknown return type", {})};

    cur.seek(retEnd);
    return PrototypeParseResult{Prototype{retTy, std::move(params), isVarArg}, ccSegment};
}

/// @brief Parse an optional calling convention specifier.
/// @param segment Text following the prototype and preceding attributes.
/// @param lineNo Source line used for diagnostics.
/// @return Default calling convention when absent/recognized, otherwise an error.
Expected<il::core::CallingConv> parseCallingConv(std::string_view segment, unsigned lineNo) {
    segment = trimView(segment);
    if (segment.empty())
        return il::core::CallingConv::Default;

    static constexpr std::array<std::pair<std::string_view, il::core::CallingConv>, 1>
        kCallingConvs = {{
            {"default", il::core::CallingConv::Default},
    }};

    for (const auto &entry : kCallingConvs) {
        if (segment == entry.first)
            return entry.second;
    }

    std::ostringstream oss;
    oss << "unknown calling convention '" << segment << "'";
    return lineError<il::core::CallingConv>(lineNo, oss.str());
}

/// @brief Parse function attributes before the opening brace or declaration end.
/// @param cur Header cursor positioned at the optional attribute list.
/// @param requireBrace True for definitions that must end the header with `{`.
/// @return Parsed attributes or a structured delimiter/name diagnostic.
Expected<Attrs> parseAttributes(Cursor &cur, bool requireBrace) {
    cur.skipWs();
    if (cur.atEnd()) {
        if (!requireBrace)
            return Attrs{};
        return Expected<Attrs>{makeSyntaxError(cursorPos(cur), "unexpected end of header", {})};
    }
    Attrs attrs;
    if (cur.peek() == '[') {
        cur.consume('[');
        const size_t attrsBegin = cur.offset();
        const size_t close = cur.view().find(']', attrsBegin);
        if (close == std::string_view::npos)
            return Expected<Attrs>{
                makeSyntaxError(cursorPos(cur), "malformed function attributes", {})};

        std::string body = trim(std::string(cur.view().substr(attrsBegin, close - attrsBegin)));
        if (body.empty())
            return Expected<Attrs>{
                makeSyntaxError(cursorPos(cur), "empty function attribute list", {})};

        for (char &ch : body)
            if (ch == ',')
                ch = ' ';

        std::istringstream ss(body);
        std::string attr;
        while (ss >> attr) {
            if (attr == "nothrow") {
                attrs.nothrow = true;
            } else if (attr == "readonly") {
                attrs.readonly = true;
            } else if (attr == "pure") {
                attrs.pure = true;
            } else if (attr == "module_init") {
                attrs.moduleInitializer = true;
            } else {
                std::ostringstream oss;
                oss << "unknown function attribute '" << attr << "'";
                return lineError<Attrs>(cur.line(), oss.str());
            }
        }
        cur.seek(close + 1);
        cur.skipWs();
    }
    if (!requireBrace)
        return attrs;
    if (!cur.consume('{'))
        return Expected<Attrs>{makeSyntaxError(cursorPos(cur), "malformed function header", {})};
    return attrs;
}

/// @brief Parse an optional source location directive.
/// @param cur Header cursor positioned after attributes.
/// @return Parsed location when supported, otherwise the default unknown location.
Expected<il::support::SourceLoc> parseOptionalLoc(Cursor &cur) {
    cur.skipWs();
    return il::support::SourceLoc{};
}

/// @brief Test whether a function-header suffix is empty or comment-only.
/// @param text Unconsumed header suffix.
/// @return True for whitespace, `//` comments, or semicolon comments.
bool isIgnorableTrailing(std::string_view text) {
    text = trimView(text);
    return text.empty() || text.rfind("//", 0) == 0 || text.front() == ';';
}

} // namespace

// ============================================================================
// Public API
// ============================================================================

/// @brief Parse and transactionally append one function declaration header.
/// @param header Complete `func` line.
/// @param st Mutable parser state and destination module.
/// @return Success with the new function active, or a structured diagnostic
///         with all state/module mutations rolled back.
Expected<void> parseFunctionHeader(const std::string &header, ParserState &st) {
    ParserSnapshot snapshot{st};
    Cursor cursor{header, SourcePos{st.lineNo, 0}};

    // Parse optional linkage keyword between "func" and "@name".
    // The header string starts with "func " already consumed by the caller's
    // keyword check, but the cursor still sees the full line.
    il::core::Linkage linkage = il::core::Linkage::Internal;
    {
        // Find the '@' that starts the function name.
        size_t atPos = header.find('@');
        if (atPos != std::string::npos && atPos >= 5) {
            // Extract text between "func " and "@"
            std::string_view between = trimView(std::string_view(header).substr(4, atPos - 4));
            if (between.empty()) {
                linkage = il::core::Linkage::Internal;
            } else if (between == "export") {
                linkage = il::core::Linkage::Export;
            } else if (between == "import") {
                linkage = il::core::Linkage::Import;
            } else {
                std::ostringstream oss;
                oss << "unknown function linkage '" << between << "'";
                return lineError<void>(st.lineNo, oss.str());
            }
        }
    }

    const bool isImport = (linkage == il::core::Linkage::Import);

    FunctionHeader fh;
    {
        auto name = parseSymbolName(cursor);
        if (!name)
            return Expected<void>{name.error()};
        fh.name = std::move(name.value());
    }
    {
        auto proto = parsePrototype(cursor, isImport);
        if (!proto)
            return Expected<void>{proto.error()};
        auto parsedProto = std::move(proto.value());
        fh.proto = std::move(parsedProto.proto);
        auto cc = parseCallingConv(parsedProto.callingConvSegment, st.lineNo);
        if (!cc)
            return Expected<void>{cc.error()};
        fh.cc = cc.value();
    }
    {
        auto attrs = parseAttributes(cursor, !isImport);
        if (!attrs)
            return Expected<void>{attrs.error()};
        fh.attrs = attrs.value();
    }
    {
        auto loc = parseOptionalLoc(cursor);
        if (!loc)
            return Expected<void>{loc.error()};
        fh.loc = loc.value();
    }
    if (!isIgnorableTrailing(cursor.remaining()))
        return lineError<void>(st.lineNo, "unexpected characters after function header");

    if (st.functionNames.contains(fh.name)) {
        std::ostringstream oss;
        oss << "duplicate function '@" << fh.name << "'";
        return lineError<void>(st.lineNo, oss.str());
    }
    if (st.externNames.contains(fh.name)) {
        std::ostringstream oss;
        oss << "function '@" << fh.name << "' collides with extern";
        return lineError<void>(st.lineNo, oss.str());
    }
    if (st.globalNames.contains(fh.name)) {
        std::ostringstream oss;
        oss << "function '@" << fh.name << "' collides with global";
        return lineError<void>(st.lineNo, oss.str());
    }

    std::unordered_set<std::string> seenParams;
    for (const auto &param : fh.proto.params) {
        if (!seenParams.insert(param.name).second) {
            std::ostringstream oss;
            oss << "duplicate parameter name '%" << param.name << "'";
            return lineError<void>(st.lineNo, oss.str());
        }
    }

    st.curLoc = fh.loc;
    st.tempIds.clear();
    st.forwardTempNames.clear();
    unsigned nextId = 0;
    for (auto &param : fh.proto.params) {
        if (static_cast<std::size_t>(nextId) >= st.limits.maxTempsPerFunction)
            return lineError<void>(st.lineNo,
                                   "resource limit exceeded: function temporaries");
        param.id = nextId;
        st.tempIds[param.name] = nextId;
        ++nextId;
    }
    st.nextTemp = nextId;

    il::core::Function fn;
    fn.name = fh.name;
    fn.nameSymbol = st.m.internIdentifier(fh.name);
    fn.retType = fh.proto.retType;
    fn.params = std::move(fh.proto.params);
    fn.isVarArg = fh.proto.isVarArg;
    fn.callingConv = fh.cc;
    fn.linkage = linkage;
    fn.moduleInitializer = fh.attrs.moduleInitializer;
    fn.attrs().nothrow = fh.attrs.nothrow;
    fn.attrs().readonly = fh.attrs.readonly;
    fn.attrs().pure = fh.attrs.pure;
    st.m.functions.push_back(std::move(fn));
    st.functionNames.insert(fh.name);
    st.curFn = &st.m.functions.back();
    st.curBB = nullptr;
    st.curFn->valueNames.resize(st.nextTemp);
    for (const auto &param : st.curFn->params)
        st.curFn->valueNames[param.id] = param.name;
    st.blockParamCount.clear();
    st.pendingBrs.clear();

    snapshot.discard();
    return {};
}

} // namespace il::io::detail
