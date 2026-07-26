//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Parser_Decl.cpp
/// @brief Declaration parsing implementation for the Zia parser.
///
/// @details Parses module headers and binds, dispatches top-level declarations,
///          and builds functions, nominal types, namespaces, aliases, globals,
///          members, properties, and enums. Shared helpers handle generic
///          constraints, parameters, visibility/modifier validation, qualified
///          names, and interface lists.
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Parser.hpp"

namespace il::frontends::zia {

//===----------------------------------------------------------------------===//
// Declaration Parsing
//===----------------------------------------------------------------------===//

/// @brief Parse a top-level module declaration (module Name; binds; declarations).
/// @return The parsed ModuleDecl, or nullptr on error.
std::unique_ptr<ModuleDecl> Parser::parseModule() {
    SourceLoc loc = peek().loc;
    std::string name = "Main";
    if (check(TokenKind::KwModule)) {
        Token moduleTok;
        if (!expect(TokenKind::KwModule, "module", &moduleTok))
            return nullptr;
        loc = moduleTok.loc;

        Token nameTok;
        if (!expect(TokenKind::Identifier, "module name", &nameTok))
            return nullptr;
        name = nameTok.text;

        if (!expect(TokenKind::Semicolon, ";"))
            return nullptr;
    }

    auto module = std::make_unique<ModuleDecl>(loc, std::move(name));

    // Parse binds
    while (check(TokenKind::KwBind)) {
        module->binds.push_back(parseBindDecl());
    }

    // Parse declarations
    while (!check(TokenKind::Eof)) {
        // Skip any stray closing braces left over from error recovery.
        // This prevents infinite loops when parse errors leave unmatched braces.
        if (check(TokenKind::RBrace)) {
            error("unexpected '}' at module level");
            advance();
            continue;
        }

        DeclPtr decl = parseDeclaration();
        if (!decl) {
            resyncAfterError();
            continue;
        }
        module->declarations.push_back(std::move(decl));
    }

    return module;
}

/// @brief Parse a bind declaration for file or namespace imports.
/// @details Supports string literal paths, dotted namespace paths, selective imports
///          ({ Item1, Item2 }), and alias imports (as T).
/// @return The parsed BindDecl.
BindDecl Parser::parseBindDecl() {
    Token bindTok = advance(); // consume 'bind'
    SourceLoc loc = bindTok.loc;

    std::string path;
    std::string alias;
    bool isNamespaceBind = false;
    bool targetErrorReported = false;

    /// @brief Parses a string path or dotted namespace bind target.
    /// @param[out] outPath Receives the parsed target spelling.
    /// @param[out] outIsNamespaceBind Receives whether the target is a namespace.
    /// @return `true` when a syntactically valid target was consumed.
    auto parseBindTarget = [&](std::string &outPath, bool &outIsNamespaceBind) -> bool {
        if (check(TokenKind::StringLiteral)) {
            Token pathTok = advance();
            outPath = pathTok.stringValue;
            outIsNamespaceBind = false;
            return true;
        }

        if (!check(TokenKind::Identifier))
            return false;

        Token firstTok = advance();
        outPath = firstTok.text;

        while (match(TokenKind::Dot)) {
            if (!check(TokenKind::Identifier)) {
                error("expected identifier in bind path");
                targetErrorReported = true;
                return false;
            }
            outPath += ".";
            Token segmentTok = advance();
            outPath += segmentTok.text;
        }

        outIsNamespaceBind = outPath.rfind("Zanna.", 0) == 0;
        return true;
    };

    // Legacy alias-first form: bind Alias = Zanna.IO;
    if (check(TokenKind::Identifier) && check(TokenKind::Equal, 1)) {
        Token aliasTok = advance();
        alias = aliasTok.text;
        advance(); // consume '='
        if (!parseBindTarget(path, isNamespaceBind)) {
            error("expected bind target after '='");
            return BindDecl(loc, "");
        }
    } else if (!parseBindTarget(path, isNamespaceBind)) {
        if (!targetErrorReported)
            error("expected bind path (string or identifier)");
        return BindDecl(loc, "");
    }

    BindDecl decl(loc, path);
    decl.isNamespaceBind = isNamespaceBind;
    decl.alias = std::move(alias);

    // Parse optional selective import: { item1, item2, ... }
    if (check(TokenKind::LBrace)) {
        advance(); // consume '{'
        while (!check(TokenKind::RBrace) && !check(TokenKind::Eof)) {
            if (!check(TokenKind::Identifier)) {
                error("expected identifier in selective import list");
                break;
            }
            Token itemTok = advance();
            decl.specificItems.push_back(itemTok.text);

            if (!match(TokenKind::Comma))
                break;
        }
        if (!expect(TokenKind::RBrace, "}"))
            return decl;
    }

    // Parse optional alias: as AliasName
    // Note: alias and selective import are mutually exclusive
    if (match(TokenKind::KwAs)) {
        if (!decl.specificItems.empty()) {
            error("cannot use alias 'as' with selective imports { ... }");
            return decl;
        }
        if (!decl.alias.empty()) {
            error("bind alias already specified before '='");
            return decl;
        }
        if (!check(TokenKind::Identifier)) {
            error("expected alias name after 'as'");
            return decl;
        }
        Token aliasTok = advance();
        decl.alias = aliasTok.text;
    }

    if (!expect(TokenKind::Semicolon, ";"))
        return decl;

    return decl;
}

/// @brief Dispatch to the appropriate declaration parser based on the current keyword.
/// @details Handles func, struct, class, interface, namespace, and var/final declarations.
/// @return The parsed declaration, or nullptr on error.
DeclPtr Parser::parseDeclaration() {
    // Module-level visibility/export modifier.
    if (check(TokenKind::KwExpose) || check(TokenKind::KwHide)) {
        bool isExported = match(TokenKind::KwExpose);
        if (!isExported)
            advance(); // consume 'hide'

        /// @brief Applies the parsed top-level visibility to a declaration.
        /// @param decl Declaration to annotate.
        /// @return The annotated declaration, or null unchanged.
        auto applyTopLevelVisibility = [&](DeclPtr decl) -> DeclPtr {
            if (!decl)
                return nullptr;
            decl->isExported = isExported;
            if (decl->kind == DeclKind::Function) {
                static_cast<FunctionDecl *>(decl.get())->visibility =
                    isExported ? Visibility::Public : Visibility::Private;
            } else if (decl->kind == DeclKind::Enum) {
                static_cast<EnumDecl *>(decl.get())->visibility =
                    isExported ? Visibility::Public : Visibility::Private;
            }
            return decl;
        };

        if (check(TokenKind::KwAsync)) {
            advance(); // consume 'async'
            if (!check(TokenKind::KwFunc)) {
                error("expected 'func' after 'async'");
                return nullptr;
            }
            auto decl = parseFunctionDecl();
            if (decl)
                static_cast<FunctionDecl *>(decl.get())->isAsync = true;
            return applyTopLevelVisibility(std::move(decl));
        }
        if (check(TokenKind::KwForeign)) {
            advance(); // consume 'foreign'
            if (!check(TokenKind::KwFunc)) {
                error("expected 'func' after 'foreign'");
                return nullptr;
            }
            auto decl = parseFunctionDecl(/*isForeign=*/true);
            if (decl)
                static_cast<FunctionDecl *>(decl.get())->isForeign = true;
            return applyTopLevelVisibility(std::move(decl));
        }
        if (check(TokenKind::KwFunc))
            return applyTopLevelVisibility(parseFunctionDecl());
        if (check(TokenKind::KwEnum))
            return applyTopLevelVisibility(parseEnumDecl());
        if (check(TokenKind::KwStruct))
            return applyTopLevelVisibility(parseStructDecl());
        if (check(TokenKind::KwClass))
            return applyTopLevelVisibility(parseClassDecl());
        if (check(TokenKind::KwInterface))
            return applyTopLevelVisibility(parseInterfaceDecl());
        if (check(TokenKind::KwNamespace))
            return applyTopLevelVisibility(parseNamespaceDecl());
        if (check(TokenKind::KwType))
            return applyTopLevelVisibility(parseTypeAlias());
        if (check(TokenKind::KwVar) || check(TokenKind::KwFinal) || check(TokenKind::KwLet))
            return applyTopLevelVisibility(parseGlobalVarDecl());

        error("expected declaration after visibility modifier");
        return nullptr;
    }
    // Foreign function import: `foreign func name(...) -> Type` (no body).
    if (check(TokenKind::KwForeign)) {
        advance(); // consume 'foreign'
        if (check(TokenKind::KwFunc)) {
            auto decl = parseFunctionDecl(/*isForeign=*/true);
            if (decl) {
                auto *fn = static_cast<FunctionDecl *>(decl.get());
                fn->isForeign = true;
            }
            return decl;
        }
        error("expected 'func' after 'foreign'");
        return nullptr;
    }
    // async func — marks function as async (returns Future)
    if (check(TokenKind::KwAsync)) {
        advance(); // consume 'async'
        if (check(TokenKind::KwFunc)) {
            auto decl = parseFunctionDecl();
            if (decl) {
                auto *fn = static_cast<FunctionDecl *>(decl.get());
                fn->isAsync = true;
            }
            return decl;
        }
        error("expected 'func' after 'async'");
        return nullptr;
    }
    if (check(TokenKind::KwFunc)) {
        return parseFunctionDecl();
    }
    if (check(TokenKind::KwStruct)) {
        return parseStructDecl();
    }
    if (check(TokenKind::KwClass)) {
        return parseClassDecl();
    }
    if (check(TokenKind::KwInterface)) {
        return parseInterfaceDecl();
    }
    if (check(TokenKind::KwEnum)) {
        return parseEnumDecl();
    }
    if (check(TokenKind::KwNamespace)) {
        return parseNamespaceDecl();
    }
    if (check(TokenKind::KwType)) {
        return parseTypeAlias();
    }
    // Module-level variable declarations (global variables)
    if (check(TokenKind::KwVar) || check(TokenKind::KwFinal) || check(TokenKind::KwLet)) {
        return parseGlobalVarDecl();
    }
    if (check(TokenKind::At)) {
        error("attributes ('@') are not supported in Zia");
        return nullptr;
    }
    error("expected declaration");
    return nullptr;
}

/// @brief Parse a function declaration (func name[T](...) -> RetType { body }).
/// @param isForeign If true, this is a foreign import — no body is expected.
/// @return The parsed FunctionDecl, or nullptr on error.
DeclPtr Parser::parseFunctionDecl(bool isForeign) {
    Token funcTok = advance(); // consume 'func'
    SourceLoc loc = funcTok.loc;

    if (!check(TokenKind::Identifier)) {
        error("expected function name");
        return nullptr;
    }
    Token nameTok = advance();
    std::string name = nameTok.text;

    auto func = std::make_unique<FunctionDecl>(loc, std::move(name));
    func->isForeign = isForeign;

    // Generic parameters with optional constraints
    func->genericParams = parseGenericParamsWithConstraints(func->genericParamConstraints);

    // Parameters
    if (!expect(TokenKind::LParen, "("))
        return nullptr;

    if (!parseParameters(func->params))
        return nullptr;

    if (!expect(TokenKind::RParen, ")"))
        return nullptr;

    // Return type
    if (match(TokenKind::Arrow)) {
        func->returnType = parseType();
        if (!func->returnType)
            return nullptr;
    }

    // Body — foreign functions have no body (they are import declarations).
    if (isForeign) {
        // No body expected — function is defined in another module. The
        // documented spelling ends with a semicolon; keep it optional so older
        // imports without one continue to parse.
        if (check(TokenKind::Semicolon))
            advance();
        return func;
    }

    if (check(TokenKind::LBrace)) {
        func->body = parseBlock();
        if (!func->body)
            return nullptr;
    } else if (match(TokenKind::Equal)) {
        // Single-expression function: func f(x: T) -> R = expr;
        SourceLoc exprLoc = peek().loc;
        ExprPtr bodyExpr = parseExpressionAllowingStructLiterals();
        if (!bodyExpr)
            return nullptr;
        if (!expect(TokenKind::Semicolon, "';'"))
            return nullptr;
        auto returnStmt = std::make_unique<ReturnStmt>(exprLoc, std::move(bodyExpr));
        std::vector<StmtPtr> stmts;
        stmts.push_back(std::move(returnStmt));
        func->body = std::make_unique<BlockStmt>(exprLoc, std::move(stmts));
    } else {
        error("expected function body");
        return nullptr;
    }

    return func;
}

/// @brief Parse a comma-separated list of function or method parameters.
/// @param[out] params Destination vector populated in declaration order.
/// @details Supports `name: Type` parameters, including optional types (`Type?`),
///          generics (`List[T]`), variadics, and default values.
/// @return True on success, false on parse error.
bool Parser::parseParameters(std::vector<Param> &params) {
    if (check(TokenKind::RParen)) {
        return true;
    }

    while (true) {
        Param param;

        if (!checkIdentifierLike()) {
            error("expected parameter");
            return false;
        }

        // Read first identifier (may be a contextual keyword like 'value')
        Token firstTok = advance();
        std::string first = firstTok.text;
        SourceLoc firstLoc = firstTok.loc;

        if (!match(TokenKind::Colon)) {
            error("expected ':' after parameter name");
            return false;
        }
        param.name = first;
        param.loc = firstLoc;
        param.isVariadic = match(TokenKind::Ellipsis);
        param.type = parseType();
        if (!param.type)
            return false;

        // Default value
        if (match(TokenKind::Equal)) {
            param.defaultValue = parseExpressionAllowingStructLiterals();
            if (!param.defaultValue)
                return false;
        }

        params.push_back(std::move(param));
        if (!match(TokenKind::Comma))
            break;
        if (check(TokenKind::RParen))
            break;
    }

    // Validate: only the last parameter can be variadic
    bool sawDefault = false;
    for (size_t i = 0; i + 1 < params.size(); ++i) {
        if (params[i].isVariadic) {
            error("Only the last parameter can be variadic");
        }
        if (params[i].defaultValue)
            sawDefault = true;
        else if (sawDefault) {
            errorAt(params[i].loc, "Required parameters cannot follow default parameters");
        }
    }
    if (!params.empty()) {
        const Param &last = params.back();
        if (last.isVariadic && last.defaultValue)
            errorAt(last.loc, "Variadic parameters cannot have default values");
        if (!last.defaultValue && !last.isVariadic && sawDefault)
            errorAt(last.loc, "Required parameters cannot follow default parameters");
    }

    return true;
}

/// @brief Parse and return a complete parameter list.
/// @return The parsed parameter list, or empty vector on error.
std::vector<Param> Parser::parseParameters() {
    std::vector<Param> params;
    if (!parseParameters(params))
        return {};
    return params;
}

/// @brief Parse generic type parameters enclosed in brackets ([T, U, ...]).
/// @return The list of type parameter names, or empty if no brackets present.
std::vector<std::string> Parser::parseGenericParams() {
    std::vector<std::string> params;

    if (!match(TokenKind::LBracket)) {
        return params;
    }

    do {
        if (!check(TokenKind::Identifier)) {
            error("expected type parameter name");
            return {};
        }
        Token nameTok = advance();
        params.push_back(nameTok.text);
    } while (match(TokenKind::Comma));

    if (!expect(TokenKind::RBracket, "]"))
        return {};

    return params;
}

/// @brief Parse generic type parameters with optional constraints ([T: Comparable, U]).
/// @param[out] constraints Parallel vector of constraint interface names (empty string =
/// unconstrained).
/// @return The list of type parameter names, or empty if no brackets present.
std::vector<std::string> Parser::parseGenericParamsWithConstraints(
    std::vector<std::string> &constraints) {
    std::vector<std::string> params;
    constraints.clear();

    if (!match(TokenKind::LBracket)) {
        return params;
    }

    do {
        if (!check(TokenKind::Identifier)) {
            error("expected type parameter name");
            return {};
        }
        Token nameTok = advance();
        params.push_back(nameTok.text);

        // Check for optional constraint: T: ConstraintName
        if (match(TokenKind::Colon)) {
            std::string constraint = parseQualifiedIdentifierString("constraint interface name");
            if (constraint.empty())
                return {};
            constraints.push_back(std::move(constraint));
        } else {
            constraints.push_back(""); // No constraint
        }
    } while (match(TokenKind::Comma));

    if (!expect(TokenKind::RBracket, "]"))
        return {};

    return params;
}

/// @brief Parse a dot-separated identifier path.
/// @param what Human-readable construct name used in diagnostics.
/// @return Parsed qualified name, or an empty string on error.
std::string Parser::parseQualifiedIdentifierString(const char *what) {
    if (!check(TokenKind::Identifier)) {
        error(std::string("expected ") + what);
        return "";
    }

    std::string name = advance().text;
    while (match(TokenKind::Dot)) {
        if (!check(TokenKind::Identifier)) {
            error(std::string("expected identifier after '.' in ") + what);
            return "";
        }
        name += ".";
        name += advance().text;
    }
    return name;
}

/// @brief Parse a comma-separated interface list after 'implements'.
/// @param[out] interfaces Output vector of interface name strings.
/// @return True on success (or no 'implements' present), false on error.
bool Parser::parseInterfaceList(std::vector<std::string> &interfaces) {
    if (!match(TokenKind::KwImplements))
        return true;

    do {
        std::string ifaceName = parseQualifiedIdentifierString("interface name");
        if (ifaceName.empty())
            return false;
        interfaces.push_back(std::move(ifaceName));
    } while (match(TokenKind::Comma));

    return true;
}

/// @brief Parse type body members (fields and methods) between braces.
/// @param[out] members Output member list.
/// @param defaultVisibility Default visibility (Public for struct, Private for class).
/// @param allowOverride Whether the 'override' modifier is permitted.
/// @return True on success.
bool Parser::parseMemberBlock(std::vector<DeclPtr> &members,
                              Visibility defaultVisibility,
                              bool allowOverride) {
    while (!check(TokenKind::RBrace) && !check(TokenKind::Eof)) {
        Visibility visibility = defaultVisibility;
        bool visibilityExplicit = false;
        bool isOverride = false;
        bool isStatic = false;
        bool isWeak = false;

        // Parse modifiers (can appear in any order when override is allowed)
        while (check(TokenKind::KwExpose) || check(TokenKind::KwHide) ||
               (allowOverride && check(TokenKind::KwOverride)) || check(TokenKind::KwStatic) ||
               check(TokenKind::KwWeak)) {
            if (match(TokenKind::KwExpose)) {
                visibility = Visibility::Public;
                visibilityExplicit = true;
            } else if (match(TokenKind::KwHide)) {
                visibility = Visibility::Private;
                visibilityExplicit = true;
            } else if (allowOverride && match(TokenKind::KwOverride))
                isOverride = true;
            else if (match(TokenKind::KwStatic))
                isStatic = true;
            else if (match(TokenKind::KwWeak))
                isWeak = true;
        }

        if (check(TokenKind::KwFunc)) {
            if (isWeak)
                error("'weak' can only be used on fields");
            if (isStatic && isOverride) {
                error("'static' methods cannot be marked 'override'");
                isOverride = false;
            }
            auto method = parseMethodDecl(/*allowBodylessSignature=*/false);
            if (method) {
                auto *m = static_cast<MethodDecl *>(method.get());
                m->visibility = visibility;
                m->isOverride = isOverride;
                m->isStatic = isStatic;
                members.push_back(std::move(method));
            }
        } else if (check(TokenKind::KwProperty)) {
            if (isWeak)
                error("'weak' can only be used on fields");
            if (isOverride) {
                error("'override' can only be used on methods");
                isOverride = false;
            }
            if (isStatic) {
                error("'static' cannot be applied to properties; use a static method or a "
                      "static field instead");
                isStatic = false;
            }
            auto prop = parsePropertyDecl();
            if (prop) {
                auto *p = static_cast<PropertyDecl *>(prop.get());
                p->visibility = visibility;
                p->isStatic = false;
                members.push_back(std::move(prop));
            }
        } else if (check(TokenKind::KwDeinit)) {
            if (visibilityExplicit || isOverride || isStatic || isWeak)
                error("'deinit' declarations cannot use expose, hide, override, static, or weak");
            Token deinitTok = advance(); // consume 'deinit'
            auto dtor = std::make_unique<DestructorDecl>(deinitTok.loc);

            // parseBlock() expects and consumes '{' itself
            dtor->body = parseBlock();
            members.push_back(std::move(dtor));
        } else if (check(TokenKind::Identifier) || check(TokenKind::KwVar) ||
                   check(TokenKind::KwFinal) || check(TokenKind::KwLet)) {
            if (isOverride) {
                error("'override' can only be used on methods");
                isOverride = false;
            }
            auto field = parseFieldDecl();
            if (field) {
                auto *f = static_cast<FieldDecl *>(field.get());
                f->visibility = visibility;
                f->isStatic = isStatic;
                f->isWeak = isWeak;
                if (f->isFinal && f->isWeak)
                    error("'weak' cannot be combined with 'final'");
                members.push_back(std::move(field));
            }
        } else {
            error("expected field, method, property, or deinit declaration");
            advance();
        }
    }

    return true;
}

/// @brief Parse a struct type declaration (struct Name[T] implements I { fields; methods }).
/// @return The parsed StructDecl, or nullptr on error.
DeclPtr Parser::parseStructDecl() {
    Token structTok = advance(); // consume 'struct'
    SourceLoc loc = structTok.loc;

    if (!check(TokenKind::Identifier)) {
        error("expected struct type name");
        return nullptr;
    }
    Token nameTok = advance();
    std::string name = nameTok.text;

    auto decl = std::make_unique<StructDecl>(loc, std::move(name));

    // Generic parameters
    decl->genericParams = parseGenericParamsWithConstraints(decl->genericParamConstraints);

    // Implements clause
    if (!parseInterfaceList(decl->interfaces))
        return nullptr;

    // Body
    if (!expect(TokenKind::LBrace, "{"))
        return nullptr;

    parseMemberBlock(decl->members, Visibility::Public, /*allowOverride=*/false);

    if (!expect(TokenKind::RBrace, "}"))
        return nullptr;

    return decl;
}

/// @brief Parse a class type declaration (class Name[T] extends Base implements I { ... }).
/// @return The parsed ClassDecl, or nullptr on error.
DeclPtr Parser::parseClassDecl() {
    Token classTok = advance(); // consume 'class'
    SourceLoc loc = classTok.loc;

    if (!check(TokenKind::Identifier)) {
        error("expected class type name");
        return nullptr;
    }
    Token nameTok = advance();
    std::string name = nameTok.text;

    auto decl = std::make_unique<ClassDecl>(loc, std::move(name));

    // Generic parameters
    decl->genericParams = parseGenericParamsWithConstraints(decl->genericParamConstraints);

    // Extends clause
    if (match(TokenKind::KwExtends)) {
        std::string baseName = parseQualifiedIdentifierString("base class name");
        if (baseName.empty())
            return nullptr;
        decl->baseClass = std::move(baseName);
    }

    // Implements clause
    if (!parseInterfaceList(decl->interfaces))
        return nullptr;

    // Body
    if (!expect(TokenKind::LBrace, "{"))
        return nullptr;

    parseMemberBlock(decl->members, Visibility::Private, /*allowOverride=*/true);

    if (!expect(TokenKind::RBrace, "}"))
        return nullptr;

    return decl;
}

/// @brief Parse an interface declaration (interface Name[T] { method signatures }).
/// @return The parsed InterfaceDecl, or nullptr on error.
DeclPtr Parser::parseInterfaceDecl() {
    Token ifaceTok = advance(); // consume 'interface'
    SourceLoc loc = ifaceTok.loc;

    if (!check(TokenKind::Identifier)) {
        error("expected interface name");
        return nullptr;
    }
    Token nameTok = advance();
    std::string name = nameTok.text;

    auto iface = std::make_unique<InterfaceDecl>(loc, std::move(name));

    // Generic parameters
    iface->genericParams = parseGenericParamsWithConstraints(iface->genericParamConstraints);

    // Body
    if (!expect(TokenKind::LBrace, "{"))
        return nullptr;

    // Parse method signatures
    while (!check(TokenKind::RBrace) && !check(TokenKind::Eof)) {
        if (check(TokenKind::KwFunc)) {
            // Parse method signature (method without body)
            auto method = parseMethodDecl(/*allowBodylessSignature=*/true);
            if (method) {
                static_cast<MethodDecl *>(method.get())->visibility = Visibility::Public;
                iface->members.push_back(std::move(method));
            }
        } else {
            error("expected method signature in interface");
            advance();
        }
    }

    if (!expect(TokenKind::RBrace, "}"))
        return nullptr;

    return iface;
}

/// @brief Parse a type alias declaration (`type Name = Target;`).
/// @return The parsed TypeAliasDecl, or nullptr on error.
DeclPtr Parser::parseTypeAlias() {
    SourceLoc loc = peek().loc;
    advance(); // consume 'type'

    if (!check(TokenKind::Identifier)) {
        error("expected type alias name");
        return nullptr;
    }
    std::string name = advance().text;

    if (!expect(TokenKind::Equal, "'='"))
        return nullptr;

    TypePtr target = parseType();
    if (!target)
        return nullptr;

    if (!expect(TokenKind::Semicolon, "';'"))
        return nullptr;

    return std::make_unique<TypeAliasDecl>(loc, std::move(name), std::move(target));
}

/// @brief Parse a namespace declaration with nested declarations.
/// @return The parsed NamespaceDecl, or nullptr on error.
/// @details Accepts qualified namespace names and enforces the shared
///          statement/namespace nesting limit while recovering from malformed
///          nested declarations.
DeclPtr Parser::parseNamespaceDecl() {
    if (++stmtDepth_ > kMaxStmtDepth) {
        --stmtDepth_;
        error("namespace nesting too deep (limit: 512)");
        return nullptr;
    }

    struct DepthGuard {
        unsigned &d;

        /// @brief Restore the namespace-recursion counter on every exit path.
        ~DepthGuard() {
            --d;
        }
    } nsGuard_{stmtDepth_};

    Token nsTok = advance(); // consume 'namespace'
    SourceLoc loc = nsTok.loc;

    // Parse namespace name (can be dotted like MyLib.Internal)
    if (!check(TokenKind::Identifier)) {
        error("expected namespace name");
        return nullptr;
    }

    std::string name = advance().text;

    // Allow dotted names: namespace Foo.Bar.Baz { }
    while (check(TokenKind::Dot)) {
        advance(); // consume '.'
        if (!check(TokenKind::Identifier)) {
            error("expected identifier after '.' in namespace name");
            return nullptr;
        }
        name += ".";
        name += advance().text;
    }

    if (!expect(TokenKind::LBrace, "{"))
        return nullptr;

    auto ns = std::make_unique<NamespaceDecl>(loc, name);

    // Parse declarations inside the namespace
    while (!check(TokenKind::RBrace) && !check(TokenKind::Eof)) {
        if (DeclPtr decl = parseDeclaration()) {
            ns->declarations.push_back(std::move(decl));
        } else {
            // Skip to recover
            advance();
        }
    }

    if (!expect(TokenKind::RBrace, "}"))
        return nullptr;

    return ns;
}

/// @brief Parse a global variable declaration using var/final/let syntax.
/// @return The parsed GlobalVarDecl, or nullptr on error.
DeclPtr Parser::parseGlobalVarDecl() {
    Token kwTok = advance(); // consume 'var', 'final', or 'let'
    SourceLoc loc = kwTok.loc;
    bool isFinal = kwTok.kind == TokenKind::KwFinal || kwTok.kind == TokenKind::KwLet;

    if (!check(TokenKind::Identifier)) {
        error("expected variable name");
        return nullptr;
    }
    Token nameTok = advance();
    std::string name = nameTok.text;

    auto decl = std::make_unique<GlobalVarDecl>(loc, std::move(name));
    decl->isFinal = isFinal;

    // Optional type annotation: var x: Integer
    if (match(TokenKind::Colon)) {
        decl->type = parseType();
        if (!decl->type)
            return nullptr;
    }

    // Optional initializer: var x = 42
    if (match(TokenKind::Equal)) {
        decl->initializer = parseExpressionAllowingStructLiterals();
        if (!decl->initializer)
            return nullptr;
    }

    if (!expect(TokenKind::Semicolon, ";"))
        return nullptr;

    return decl;
}

/// @brief Parse a field declaration inside a struct or class body.
/// @details Supports both `Type name;` and `name: Type;`.
/// @return The parsed FieldDecl, or nullptr on error.
DeclPtr Parser::parseFieldDecl() {
    SourceLoc loc = peek().loc;
    // Optional storage-class prefix: `var` (mutable), `final`/`let` (write-once,
    // assignable only inside init).
    bool isFinal = false;
    if (check(TokenKind::KwFinal) || check(TokenKind::KwLet)) {
        isFinal = true;
        advance();
        loc = peek().loc;
    } else if (match(TokenKind::KwVar)) {
        loc = peek().loc;
    }

    TypePtr type;
    std::string fieldName;

    // Familiar field form: name: Type
    if (checkIdentifierLike() && check(TokenKind::Colon, 1)) {
        Token nameTok = advance();
        fieldName = nameTok.text;
        advance(); // consume ':'
        type = parseType();
        if (!type)
            return nullptr;
    } else {
        // Original field form: Type name
        type = parseType();
        if (!type)
            return nullptr;

        if (!checkIdentifierLike()) {
            error("expected field name");
            return nullptr;
        }
        Token nameTok = advance();
        fieldName = nameTok.text;
    }

    auto field = std::make_unique<FieldDecl>(loc, std::move(fieldName));
    field->type = std::move(type);
    field->isFinal = isFinal;

    // Optional initializer: = expr
    if (match(TokenKind::Equal)) {
        field->initializer = parseExpressionAllowingStructLiterals();
        if (!field->initializer)
            return nullptr;
    }

    // Expect semicolon
    if (!expect(TokenKind::Semicolon, ";"))
        return nullptr;

    return field;
}

/// @brief Parse a method declaration inside a struct, class, or interface body.
/// @details For interfaces, the method has no body and ends with a semicolon.
/// @param allowBodylessSignature Whether a semicolon-terminated signature is accepted.
/// @return The parsed MethodDecl, or nullptr on error.
DeclPtr Parser::parseMethodDecl(bool allowBodylessSignature) {
    Token funcTok = advance(); // consume 'func'
    SourceLoc loc = funcTok.loc;

    if (!check(TokenKind::Identifier)) {
        error("expected method name");
        return nullptr;
    }
    Token nameTok = advance();
    std::string name = nameTok.text;

    auto method = std::make_unique<MethodDecl>(loc, std::move(name));

    // Generic parameters
    method->genericParams = parseGenericParamsWithConstraints(method->genericParamConstraints);

    // Parameters
    if (!expect(TokenKind::LParen, "("))
        return nullptr;
    if (!parseParameters(method->params))
        return nullptr;
    if (!expect(TokenKind::RParen, ")"))
        return nullptr;

    // Return type
    if (match(TokenKind::Arrow)) {
        method->returnType = parseType();
        if (!method->returnType)
            return nullptr;
    }

    // Body
    if (check(TokenKind::LBrace)) {
        method->body = parseBlock();
    } else if (check(TokenKind::Equal)) {
        // Single-expression method: func f(x: T) -> R = expr;
        advance();
        SourceLoc exprLoc = peek().loc;
        ExprPtr bodyExpr = parseExpressionAllowingStructLiterals();
        if (!bodyExpr)
            return nullptr;
        if (!expect(TokenKind::Semicolon, "';'"))
            return nullptr;
        auto returnStmt = std::make_unique<ReturnStmt>(exprLoc, std::move(bodyExpr));
        std::vector<StmtPtr> stmts;
        stmts.push_back(std::move(returnStmt));
        method->body = std::make_unique<BlockStmt>(exprLoc, std::move(stmts));
    } else {
        // No body - interface method signature
        if (!allowBodylessSignature) {
            errorAt(method->loc, "method declarations in class or struct bodies require a body");
            if (check(TokenKind::Semicolon))
                advance();
            return nullptr;
        }
        if (!expect(TokenKind::Semicolon, ";"))
            return nullptr;
    }

    return method;
}

/// @brief Parse a property declaration: property name: Type { get { ... } set(param) { ... } }
/// @return The parsed PropertyDecl, or nullptr on error.
DeclPtr Parser::parsePropertyDecl() {
    Token propTok = advance(); // consume 'property'
    SourceLoc loc = propTok.loc;

    // Property name
    if (!check(TokenKind::Identifier)) {
        error("expected property name");
        return nullptr;
    }
    Token nameTok = advance();
    std::string name = nameTok.text;

    // Colon + type
    if (!expect(TokenKind::Colon, ":"))
        return nullptr;
    TypePtr type = parseType();
    if (!type)
        return nullptr;

    auto prop = std::make_unique<PropertyDecl>(loc, std::move(name));
    prop->type = std::move(type);

    // Opening brace for accessor block
    if (!expect(TokenKind::LBrace, "{"))
        return nullptr;

    // Parse get/set accessors (contextual keywords — parsed as identifiers)
    bool seenGetter = false;
    bool seenSetter = false;
    while (!check(TokenKind::RBrace) && !check(TokenKind::Eof)) {
        if (check(TokenKind::Identifier) && peek().text == "get") {
            const bool duplicate = seenGetter;
            if (duplicate)
                errorAt(peek().loc, "duplicate get accessor in property declaration");
            seenGetter = true;
            advance(); // consume 'get'
            StmtPtr body = parseBlock();
            if (!duplicate)
                prop->getterBody = std::move(body);
        } else if (check(TokenKind::Identifier) && peek().text == "set") {
            const bool duplicate = seenSetter;
            if (duplicate)
                errorAt(peek().loc, "duplicate set accessor in property declaration");
            seenSetter = true;
            advance(); // consume 'set'
            // Optional parameter name: set(value)
            if (match(TokenKind::LParen)) {
                if (check(TokenKind::Identifier)) {
                    prop->setterParam = advance().text;
                } else {
                    error("expected setter parameter name");
                    return nullptr;
                }
                if (!expect(TokenKind::RParen, ")"))
                    return nullptr;
            }
            StmtPtr body = parseBlock();
            if (!duplicate)
                prop->setterBody = std::move(body);
        } else {
            error("expected 'get' or 'set' in property declaration");
            advance();
        }
    }

    // Closing brace
    if (!expect(TokenKind::RBrace, "}"))
        return nullptr;

    if (!prop->getterBody && !prop->setterBody) {
        error("property must declare at least one accessor");
        return nullptr;
    }

    return prop;
}

/// @brief Parse an enum declaration and its optional explicit variant values.
/// @return The parsed EnumDecl, or nullptr on error.
DeclPtr Parser::parseEnumDecl() {
    auto loc = peek().loc;
    expect(TokenKind::KwEnum, "enum");

    Token nameTok;
    if (!expect(TokenKind::Identifier, "enum name", &nameTok))
        return nullptr;

    auto decl = std::make_unique<EnumDecl>(loc, nameTok.text);

    if (!expect(TokenKind::LBrace, "'{'"))
        return nullptr;

    // Parse comma-separated variants until '}'
    while (!check(TokenKind::RBrace) && !check(TokenKind::Eof)) {
        Token variantTok;
        if (!expect(TokenKind::Identifier, "variant name", &variantTok)) {
            resyncAfterError();
            continue;
        }

        EnumVariant variant;
        variant.name = variantTok.text;
        variant.loc = variantTok.loc;

        // Optional explicit value: = <integer>
        if (match(TokenKind::Equal)) {
            // Expect an integer literal (or negative: - integer)
            bool negative = false;
            if (match(TokenKind::Minus))
                negative = true;

            Token valTok;
            if (!expect(TokenKind::IntegerLiteral, "integer value", &valTok)) {
                resyncAfterError();
                continue;
            }
            int64_t val = 0;
            if (valTok.requiresNegation) {
                if (!negative) {
                    error("integer literal 9223372036854775808 out of range (use "
                          "-9223372036854775808 for minimum signed integer)");
                    resyncAfterError();
                    continue;
                }
                val = INT64_MIN;
            } else {
                val = valTok.intValue;
                if (negative) {
                    if (val == INT64_MIN) {
                        error("enum value is out of range");
                        resyncAfterError();
                        continue;
                    }
                    val = -val;
                }
            }
            variant.explicitValue = val;
        }

        decl->variants.push_back(std::move(variant));

        // Allow trailing comma; stop at '}'
        if (!match(TokenKind::Comma)) {
            // A following identifier means the user omitted the separating comma.
            // Report it specifically and keep parsing the remaining variants.
            if (check(TokenKind::Identifier)) {
                error("expected ',' between enum variants");
                continue;
            }
            break;
        }
    }

    if (!expect(TokenKind::RBrace, "'}'"))
        return nullptr;

    return decl;
}

} // namespace il::frontends::zia
