//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Parser.hpp
/// @brief Recursive descent parser for the Zia programming language.
///
/// @details The parser consumes tokens from the lexer and builds an Abstract
/// Syntax Tree (AST) representing the program structure. It uses recursive
/// descent with precedence climbing for expression parsing.
///
/// ## Parsing Strategy
///
/// The parser uses a combination of techniques:
///
/// **Recursive Descent:**
/// Each grammar rule is implemented as a parsing method that calls other
/// parsing methods for sub-rules. For example, `parseIfStmt()` calls
/// `parseExpression()` for the condition and `parseBlock()` for the body.
///
/// **Precedence Climbing:**
/// Binary expressions are parsed using precedence climbing to handle
/// operator precedence and associativity correctly without deep recursion.
///
/// **Buffered Lookahead and Speculation:**
/// `peek(offset)` fills a token buffer on demand for multi-token decisions.
/// Ambiguous constructs use bounded speculative parsing that rolls back token
/// position and error state unless explicitly committed.
///
/// ## Operator Precedence
///
/// Binary operators are parsed with the following precedence (highest first):
///
/// | Level | Operators         | Description              |
/// |-------|-------------------|--------------------------|
/// |   1   | `()` `[]` `.` `?.`| Primary & postfix        |
/// |   2   | `!` `-` `~`       | Unary operators          |
/// |   3   | `*` `/` `%`       | Multiplicative           |
/// |   4   | `+` `-`           | Additive                 |
/// |   5   | `<` `>` `<=` `>=` | Comparison               |
/// |   6   | `==` `!=`         | Equality                 |
/// |   7   | `&&`              | Logical AND              |
/// |   8   | `||`              | Logical OR               |
/// |   9   | `??`              | Null coalesce            |
/// |  10   | `..` `..=`        | Range                    |
/// |  11   | `?` `:`           | Ternary conditional      |
/// |  12   | `=`               | Assignment               |
///
/// ## Grammar Overview
///
/// ```
/// module     = ("module" IDENT ";")? bind* declaration* EOF
/// bind       = "bind" (STRING | dotted-name) ("as" IDENT)? ";"
///
/// declaration = struct-decl | class-decl | interface-decl
///             | func-decl | global-var-decl
///
/// struct-decl = "struct" IDENT generic-params? interfaces? "{" member* "}"
/// class-decl = "class" IDENT generic-params? extends? interfaces? "{" member* "}"
/// func-decl = "func" IDENT generic-params? "(" params ")" return-type? block
///
/// statement = block | var-stmt | if-stmt | while-stmt | for-stmt
///           | return-stmt | guard-stmt | match-stmt | expr-stmt
///
/// expression = assignment (precedence climbing for binary ops)
/// ```
///
/// ## Error Recovery
///
/// On syntax errors, the parser:
/// 1. Reports the error with location and message
/// 2. Attempts to resynchronize at the next statement/declaration boundary
/// 3. Continues parsing to find additional errors
///
/// This allows reporting multiple errors in a single parse pass.
///
/// ## Usage Example
///
/// ```cpp
/// DiagnosticEngine diag;
/// Lexer lexer(source, fileId, diag);
/// Parser parser(lexer, diag);
///
/// auto module = parser.parseModule();
/// if (parser.hasError()) {
///     // Handle parse errors
/// }
/// ```
///
/// @invariant peek() always yields a token, buffering lookahead as needed.
/// @invariant Precedence climbing for expressions.
/// @invariant Error recovery at statement boundaries.
///
/// @see Lexer.hpp - Token source
/// @see AST.hpp - AST node types
/// @see Sema.hpp - Semantic analysis of AST
///
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/zia/AST.hpp"
#include "frontends/zia/Lexer.hpp"
#include "support/diagnostics.hpp"
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace il::frontends::zia {

/// @brief Recursive descent parser for Zia.
///
/// @details Consumes tokens from a Lexer and builds an AST. The parser
/// handles the complete Zia grammar including:
/// - Module structure (imports, declarations)
/// - Type declarations (struct, class, interface)
/// - Function and method declarations
/// - All statement types
/// - Full expression grammar with precedence
///
/// ## Token Consumption
///
/// The parser maintains a current token and provides methods for:
/// - `peek()`: View current token without consuming
/// - `advance()`: Consume current token and get next
/// - `check(kind)`: Test if current token matches
/// - `match(kind)`: Consume if matches, return success
/// - `expect(kind, what)`: Require specific token, error if not
///
/// ## Ownership
///
/// The parser borrows references to the Lexer and DiagnosticEngine.
/// Both must outlive the parser. The parser produces AST nodes that
/// the caller owns via unique_ptr.
///
/// @invariant peek() is always valid (may be Eof token).
/// @invariant Lexer and DiagnosticEngine outlive the parser.
class Parser {
  public:
    /// @brief Create a parser over the given lexer.
    /// @param lexer Token source for parsing.
    /// @param diag Diagnostic engine for error reporting.
    ///
    /// @details Initializes the parser and fetches the first token.
    /// The lexer and diagnostic engine are borrowed and must outlive
    /// the parser.
    Parser(Lexer &lexer, il::support::DiagnosticEngine &diag);

    /// @brief Parse a complete module.
    /// @return The parsed ModuleDecl, or nullptr on fatal error.
    ///
    /// @details Parses the complete module structure:
    /// 1. Optional `module ModuleName;` declaration
    /// 2. Zero or more bind declarations
    /// 3. Zero or more top-level declarations
    /// 4. Expects EOF at end
    ///
    /// Even on errors, attempts to return a partial AST if possible.
    std::unique_ptr<ModuleDecl> parseModule();

    /// @brief Parse a single expression (for testing).
    /// @return The parsed expression, or nullptr on error.
    ///
    /// @details Parses an expression in isolation, useful for unit testing
    /// the expression parser without full module context.
    ExprPtr parseExpression();

    /// @brief Parse a single statement (for testing).
    /// @return The parsed statement, or nullptr on error.
    ///
    /// @details Parses a statement in isolation, useful for unit testing
    /// the statement parser without full module context.
    StmtPtr parseStatement();

    /// @brief Check if any errors occurred during parsing.
    /// @return True if at least one error was reported.
    ///
    /// @details Even if errors occurred, the parser may have produced
    /// a partial AST. Check this after parsing to determine success.
    bool hasError() const {
        return hasError_;
    }

  private:
    //=========================================================================
    /// @name Token Handling
    /// @brief Methods for consuming and examining tokens.
    /// @{
    //=========================================================================

    /// @brief Peek at a token without consuming.
    /// @param offset Lookahead distance (0 = current token).
    /// @return Reference to the token.
    ///
    /// @details Tokens are buffered on demand to support multi-token lookahead
    /// and bounded backtracking during parsing.
    const Token &peek(size_t offset = 0);

    /// @brief Consume current token and advance to next.
    /// @return The token that was consumed.
    ///
    /// @details After this call, peek() returns the next token.
    Token advance();

    /// @brief Check whether the token at @p offset matches @p kind.
    /// @param kind Expected token kind.
    /// @param offset Lookahead distance from the current position.
    /// @return True when the buffered token at @p offset has @p kind.
    bool check(TokenKind kind, size_t offset = 0);

    /// @brief Check if current token can be used as an identifier.
    /// @return True if current token is an identifier or contextual keyword.
    ///
    /// @details Allows certain keywords (like 'value') to be used as
    /// identifiers in contexts like parameter names.
    bool checkIdentifierLike();

    /// @brief Check whether a token can begin an expression.
    /// @param kind Token kind to classify.
    /// @return True when @p kind is valid at the start of an expression.
    bool isExpressionStart(TokenKind kind) const;

    /// @brief Check whether the current `match` token starts a match expression/statement.
    /// @return True when lookahead identifies expression-style match syntax.
    bool isMatchExpressionAhead();

    /// @brief Check whether the current `{` is likely a block expression.
    /// @return True when bounded lookahead finds a value-producing block shape.
    bool looksLikeBlockExpression();

    /// @brief Consume current token if it matches the given kind.
    /// @param kind The token kind to match.
    /// @param out Optional pointer to receive the consumed token.
    /// @return True if matched and consumed, false otherwise.
    bool match(TokenKind kind, Token *out = nullptr);

    /// @brief Require a specific token kind.
    /// @param kind The expected token kind.
    /// @param what Human-readable description of what was expected.
    /// @param out Optional pointer to receive the consumed token.
    /// @return True if found, false if error reported.
    ///
    /// @details If the current token doesn't match, reports an error
    /// like "expected 'what', found '...'". The token is consumed on match.
    bool expect(TokenKind kind, const char *what, Token *out = nullptr);

    /// @brief Reclaim consumed tokens from the front of the lookahead buffer.
    /// @details Preserves the current logical token while limiting retained
    ///          storage during long parses.
    void compactBufferedTokens();

    /// @brief Attempt to resynchronize after a syntax error.
    ///
    /// @details Skips tokens until reaching a likely statement or
    /// declaration boundary, such as:
    /// - Semicolons
    /// - Keywords like `func`, `var`, `if`, `while`, `return`
    /// - Closing braces
    ///
    /// This enables continued parsing to find additional errors.
    void resyncAfterError();

    /// @}

    //=========================================================================
    /// @name Speculative Parsing
    /// @brief Bounded backtracking helpers for disambiguation.
    /// @{
    //=========================================================================

    /// @brief RAII helper for bounded backtracking.
    /// @details Suppresses diagnostics while active. If not committed, restores
    ///          token position and error state when destroyed.
    class Speculation {
      public:
        /// @brief Begin a speculative parse: snapshot token position and
        ///        error state, and suppress diagnostics until committed.
        /// @param parser Parser whose buffered position and diagnostics are
        ///        managed by the speculation.
        explicit Speculation(Parser &parser);

        /// @brief Roll back token/error state unless commit() was called.
        ~Speculation();

        Speculation(const Speculation &) = delete;
        Speculation &operator=(const Speculation &) = delete;

        /// @brief Accept the speculative parse and retain its token progress.
        void commit() {
            committed_ = true;
        }

      private:
        Parser &parser_;
        size_t savedPos_;
        bool savedHasError_;
        bool committed_{false};
    };

    /// @}
    //=========================================================================
    /// @name Pattern Parsing
    /// @brief Helpers for match/for-in pattern parsing.
    /// @{
    //=========================================================================

    /// @brief Parse a match pattern, falling back to expression patterns.
    /// @details Uses speculative parsing to distinguish patterns from expressions.
    /// @return Parsed match-arm pattern.
    MatchArm::Pattern parseMatchPattern();

    /// @brief Parse a non-expression pattern (wildcard, literal, binding, constructor, tuple).
    /// @param out Pattern to populate.
    /// @return True if a pattern was parsed, false otherwise.
    bool parsePatternCore(MatchArm::Pattern &out);

    /// @}
    //=========================================================================
    /// @name Error Handling
    /// @{
    //=========================================================================

    /// @brief Report a syntax error at the current token.
    /// @param message Error message describing the problem.
    ///
    /// @details Sets hasError_ and sends the error to the diagnostic engine.
    /// The error location is taken from the current token.
    void error(const std::string &message);

    /// @brief Report a syntax error at a specific location.
    /// @param loc Source location of the error.
    /// @param message Error message describing the problem.
    ///
    /// @details Used when the error location differs from the current token.
    void errorAt(SourceLoc loc, const std::string &message);

    /// @}
    //=========================================================================
    /// @name Expression Parsing
    /// @brief Methods for parsing expressions using precedence climbing.
    /// @details Each method handles one precedence level, calling the
    /// next higher precedence method for its operands.
    /// @{
    //=========================================================================

    /// @brief Parse a primary expression (literals, identifiers, parentheses).
    /// @return The parsed expression.
    ///
    /// @details Handles:
    /// - Literals: integers, floats, strings, true, false, null
    /// - Identifiers
    /// - Parenthesized expressions
    /// - List literals: `[a, b, c]`
    /// - Map/set literals: `{a: 1, b: 2}` or `{a, b, c}`
    /// - Lambda expressions: `(x: Integer) => x + 1`
    /// - If/match expressions
    ExprPtr parsePrimary();

    /// @brief Parse a leading-identifier primary: either a `Type { ... }`
    ///        struct literal (when struct literals are enabled) or a plain
    ///        identifier reference. Assumes the current token is identifier-like.
    /// @param loc Source location of the leading identifier.
    /// @return Parsed struct literal or identifier expression.
    ExprPtr parseIdentifierOrStructLiteral(SourceLoc loc);

    /// @brief Parse the remainder of a `(`-led primary: unit literal `()`,
    ///        parenthesized expression, tuple, or lambda. Assumes the opening
    ///        `(` has already been consumed.
    /// @param loc Source location of the opening parenthesis.
    /// @return Parsed unit, grouping, tuple, or lambda expression.
    ExprPtr parseParenthesizedExpr(SourceLoc loc);

    /// @brief Parse an expression while permitting `Type { ... }` struct literals.
    /// @details Used for expression subcontexts that are already unambiguously value
    /// positions, while statement conditions keep struct literals disabled so
    /// braces remain block delimiters.
    /// @return Parsed expression.
    ExprPtr parseExpressionAllowingStructLiterals();

    /// @brief Parse a match expression.
    /// @param loc The source location of the match keyword.
    /// @return The parsed match expression.
    ExprPtr parseMatchExpression(SourceLoc loc);

    /// @brief Parse a value-producing block expression: `{ stmts; expr }`.
    /// @return Parsed block expression.
    ExprPtr parseBlockExpression();

    /// @brief Parse postfix operators (call, index, field access).
    /// @return The parsed expression with postfix operators applied.
    ///
    /// @details Handles chained postfix operations:
    /// - Function calls: `f(x, y)`
    /// - Array indexing: `a[i]`
    /// - Field access: `obj.field`
    /// - Optional chain: `obj?.field`
    /// - Try operator: `expr?`
    ExprPtr parsePostfix();

    /// @brief Continue parsing postfix operators from an existing expression.
    /// @param expr The expression to extend with postfix operators.
    /// @return The expression with additional postfix operators.
    ///
    /// @details Called from parsePostfix to handle chained operations.
    ExprPtr parsePostfixFrom(ExprPtr expr);

    /// @brief Parse postfix and binary operators from an existing expression.
    /// @param startExpr The expression to start from.
    /// @return The complete expression.
    ///
    /// @details Combines postfix and binary parsing for complex expressions.
    ExprPtr parsePostfixAndBinaryFrom(ExprPtr startExpr);

    /// @brief Continue parsing binary operators from an existing expression.
    /// @param expr The left-hand operand.
    /// @return The expression with binary operators applied.
    ///
    /// @details Uses precedence climbing to handle operator precedence.
    ExprPtr parseBinaryFrom(ExprPtr expr);

    /// @brief Parse a unary expression.
    /// @return The parsed expression.
    ///
    /// @details Handles prefix operators: `-`, `!`, `~`
    /// Falls through to postfix parsing for non-unary expressions.
    ExprPtr parseUnary();

    /// @brief Continue parsing binary operators from an already-parsed left operand.
    /// @details Shared precedence-climbing routine that folds the uniform
    /// left-associative binary operators (logical-OR through multiplicative) into
    /// @p left. This is the single source of truth for binary operator precedence,
    /// used by both the normal descent (parseCoalesce) and match-arm continuation
    /// (parseBinaryFrom). It does not handle `??`, ranges, ternary, or assignment.
    /// @param left The already-parsed left operand.
    /// @param minPrec The minimum precedence to consume (callers pass the loosest).
    /// @return The extended expression, or nullptr on error.
    ExprPtr parseBinaryRhs(ExprPtr left, int minPrec);

    /// @brief Parse null coalesce expressions (??) and everything tighter.
    /// @return The parsed expression.
    ExprPtr parseCoalesce();

    /// @brief Parse range expressions (.., ..=).
    /// @return The parsed expression.
    ExprPtr parseRange();

    /// @brief Parse ternary conditional expressions (? :).
    /// @return The parsed expression.
    ExprPtr parseTernary();

    /// @brief Parse assignment expressions (=).
    /// @return The parsed expression.
    ///
    /// @details Assignment is right-associative with lowest precedence.
    ExprPtr parseAssignment();

    /// @brief Parse a list literal: [a, b, c].
    /// @return The parsed ListLiteralExpr.
    ExprPtr parseListLiteral();

    /// @brief Parse a lambda body after parameters have been parsed.
    /// @param loc The source location of the lambda start.
    /// @param params The parsed lambda parameters.
    /// @return The parsed LambdaExpr.
    ExprPtr parseLambdaBody(SourceLoc loc, std::vector<LambdaParam> params);

    /// @brief Parse a map or set literal: {a: 1} or {a, b}.
    /// @return The parsed MapLiteralExpr or SetLiteralExpr.
    ///
    /// @details Distinguishes between maps (with `:`) and sets (without).
    ExprPtr parseMapOrSetLiteral();

    /// @brief Parse an interpolated string.
    /// @return The parsed string expression (may be concatenation).
    ///
    /// @details Handles strings with `${...}` interpolations by parsing
    /// the interpolated expressions and building concatenation expressions.
    ExprPtr parseInterpolatedString();

    /// @brief Parse function call arguments.
    /// @param[out] args Destination vector for positional or named arguments.
    /// @return True when the complete argument list parses successfully.
    ///
    /// @details Handles both positional and named arguments:
    /// - `f(1, 2)` - positional
    /// - `f(x: 1, y: 2)` - named
    /// - `f(1, y: 2)` - mixed
    bool parseCallArgs(std::vector<CallArg> &args);
    /// @brief Convenience overload returning the parsed call arguments
    ///        directly (empty on parse error).
    /// @return Parsed argument vector.
    std::vector<CallArg> parseCallArgs();

    /// @}
    //=========================================================================
    /// @name Statement Parsing
    /// @brief Methods for parsing statements.
    /// @{
    //=========================================================================

    /// @brief Parse a block statement: { ... }.
    /// @return The parsed BlockStmt.
    ///
    /// @details Parses statements until closing brace.
    /// Creates a new scope for local variables.
    StmtPtr parseBlock();

    /// @brief Parse a variable declaration: var x = 1; or final x = 1;
    /// @return The parsed VarStmt.
    ///
    /// @details Handles:
    /// - Mutable: `var x = 1;`
    /// - Immutable: `final x = 1;`
    /// - With type: `var x: Integer = 1;`
    StmtPtr parseVarDecl();

    /// @brief Parse an if statement: if (cond) { } else { }
    /// @return The parsed IfStmt.
    ///
    /// @details Handles optional else clause and else-if chains.
    StmtPtr parseIfStmt();

    /// @brief Parse a while statement: while (cond) { }
    /// @return The parsed WhileStmt.
    StmtPtr parseWhileStmt();

    /// @brief Parse a for statement (C-style or for-in).
    /// @return The parsed ForStmt or ForInStmt.
    ///
    /// @details Distinguishes between:
    /// - C-style: `for (init; cond; update) { }`
    /// - For-in: `for (x in collection) { }`
    StmtPtr parseForStmt();

    /// @brief Parse a return statement: return expr;
    /// @return The parsed ReturnStmt.
    ///
    /// @details Handles optional return value.
    StmtPtr parseReturnStmt();

    /// @brief Parse a defer statement: defer stmt; or defer { ... }.
    /// @return The parsed DeferStmt.
    StmtPtr parseDeferStmt();

    /// @brief Parse a guard statement: guard (cond) else { }
    /// @return The parsed GuardStmt.
    ///
    /// @details The else block must exit the scope (return, break, etc.).
    StmtPtr parseGuardStmt();

    /// @brief Parse a match statement: match expr { ... }
    /// @return The parsed MatchStmt.
    ///
    /// @details Parses match arms with patterns and bodies.
    StmtPtr parseMatchStmt();

    /// @brief Parse a try/catch/finally statement.
    /// @return The parsed TryStmt.
    StmtPtr parseTryStmt();

    /// @brief Parse a throw statement: throw expr;
    /// @return The parsed ThrowStmt.
    StmtPtr parseThrowStmt();

    /// @}
    //=========================================================================
    /// @name Type Parsing
    /// @brief Methods for parsing type annotations.
    /// @{
    //=========================================================================

    /// @brief Parse a type annotation.
    /// @return The parsed TypeNode.
    ///
    /// @details Handles:
    /// - Simple types: `Integer`, `String`
    /// - Generic types: `List[T]`, `Map[K, V]`
    /// - Optional types: `T?`
    /// - Function types: `(A, B) -> C`
    TypePtr parseType();

    /// @brief Parse a base type (without optional suffix).
    /// @return The parsed TypeNode.
    ///
    /// @details Called by parseType() to get the base before checking
    /// for `?` suffix.
    TypePtr parseBaseType();

    /// @}
    //=========================================================================
    /// @name Declaration Parsing
    /// @brief Methods for parsing declarations.
    /// @{
    //=========================================================================

    /// @brief Parse a top-level declaration.
    /// @return The parsed declaration, or nullptr on error.
    ///
    /// @details Dispatches to specific declaration parsers based on keyword.
    DeclPtr parseDeclaration();

    /// @brief Parse a function declaration: func name(...) { }
    /// @param isForeign If true, no body is required (import declaration).
    /// @return The parsed FunctionDecl.
    DeclPtr parseFunctionDecl(bool isForeign = false);

    /// @brief Parse a struct type declaration: struct Name { }
    /// @return The parsed StructDecl.
    DeclPtr parseStructDecl();

    /// @brief Parse an class type declaration: class Name { }
    /// @return The parsed ClassDecl.
    DeclPtr parseClassDecl();

    /// @brief Parse an interface declaration: interface Name { }
    /// @return The parsed InterfaceDecl.
    DeclPtr parseInterfaceDecl();

    /// @brief Parse an enum declaration: enum Name { Variant, ... }
    /// @return The parsed EnumDecl.
    DeclPtr parseEnumDecl();

    /// @brief Parse a namespace declaration: namespace Name { declarations }
    /// @return The parsed NamespaceDecl.
    ///
    /// @details Namespaces group declarations under a qualified name.
    /// The namespace name can be dotted (e.g., `MyLib.Internal`).
    /// Example:
    /// ```
    /// namespace MyLib {
    ///     class Parser { ... }
    ///     func parse() { ... }
    /// }
    /// ```
    DeclPtr parseNamespaceDecl();

    /// @brief Parse a type alias: type Name = TargetType;
    /// @return The parsed TypeAliasDecl.
    DeclPtr parseTypeAlias();

    /// @brief Parse a global variable declaration: var x = 1;
    /// @return The parsed GlobalVarDecl.
    DeclPtr parseGlobalVarDecl();

    /// @brief Parse a bind declaration: bind Zanna.Terminal as Term;
    /// @return The parsed BindDecl.
    BindDecl parseBindDecl();

    /// @brief Parse function/method parameters.
    /// @param[out] params Destination vector for parsed parameter declarations.
    /// @return True when the complete parameter list parses successfully.
    ///
    /// @details Parses parameter list: `(name: Type, name2: Type = default)`
    bool parseParameters(std::vector<Param> &params);
    /// @brief Convenience overload returning the parsed parameter list
    ///        directly (empty on parse error).
    /// @return Parsed parameter vector.
    std::vector<Param> parseParameters();

    /// @brief Parse generic type parameters: [T, U]
    /// @return Vector of type parameter names.
    std::vector<std::string> parseGenericParams();

    /// @brief Parse generic type parameters with optional constraints: [T, U: Comparable]
    /// @param[out] constraints Output vector for constraint names (parallel to returned names).
    /// @return Vector of type parameter names.
    std::vector<std::string> parseGenericParamsWithConstraints(
        std::vector<std::string> &constraints);

    /// @brief Parse a dotted identifier such as A.B.C and return its text.
    /// @param what Human-readable item name for diagnostics.
    /// @return Parsed dotted name, or empty on error.
    std::string parseQualifiedIdentifierString(const char *what);

    /// @brief Parse a field declaration within a type.
    /// @return The parsed FieldDecl.
    DeclPtr parseFieldDecl();

    /// @brief Parse a method declaration within a type.
    /// @param allowBodylessSignature Whether a trailing-semicolon signature is valid.
    /// @return The parsed MethodDecl.
    DeclPtr parseMethodDecl(bool allowBodylessSignature = false);

    /// @brief Parse a property declaration with get/set accessors.
    /// @return The parsed PropertyDecl.
    DeclPtr parsePropertyDecl();

    /// @brief Parse a comma-separated interface list after 'implements'.
    /// @param[out] interfaces Output vector of interface name strings.
    /// @return True on success (or no 'implements' present), false on error.
    bool parseInterfaceList(std::vector<std::string> &interfaces);

    /// @brief Parse type body members (fields and methods) between braces.
    /// @param[out] members Output member list.
    /// @param defaultVisibility Default visibility (Public for struct, Private for class).
    /// @param allowOverride Whether the 'override' modifier is permitted.
    /// @return True on success.
    bool parseMemberBlock(std::vector<DeclPtr> &members,
                          Visibility defaultVisibility,
                          bool allowOverride);

    /// @}
    //=========================================================================
    /// @name Member Variables
    /// @{
    //=========================================================================

    /// @brief Token source.
    /// @details Borrowed reference; must outlive the parser.
    Lexer &lexer_;

    /// @brief Diagnostic engine for error reporting.
    /// @details Borrowed reference; must outlive the parser.
    il::support::DiagnosticEngine &diag_;

    /// @brief Buffered token stream for multi-token lookahead.
    std::deque<Token> tokens_;

    /// @brief Current position within the token buffer.
    size_t tokenPos_{0};

    /// @brief Whether any errors have occurred during parsing.
    /// @details Set by error() and errorAt().
    bool hasError_{false};

    /// @brief Current expression nesting depth for recursion guard.
    /// @details Incremented on entry to parseUnary(), decremented on exit.
    /// Prevents stack overflow from deeply nested expressions like
    /// `-(-(-(-(...))))`.
    unsigned exprDepth_{0};

    /// @brief Maximum allowed expression nesting depth.
    static constexpr unsigned kMaxExprDepth = 256;

    /// @brief Current statement/block nesting depth for recursion guard.
    unsigned stmtDepth_{0};
    /// @brief Maximum allowed statement nesting depth.
    static constexpr unsigned kMaxStmtDepth = 512;

    /// @brief Current type annotation nesting depth for recursion guard.
    unsigned typeDepth_{0};
    /// @brief Maximum allowed type nesting depth.
    static constexpr unsigned kMaxTypeDepth = 256;

    /// @brief Current match-pattern nesting depth for recursion guard.
    unsigned patternDepth_{0};
    /// @brief Maximum allowed pattern nesting depth.
    static constexpr unsigned kMaxPatternDepth = 256;

    /// @brief Depth of speculative parsing scopes (suppresses diagnostics).
    int suppressionDepth_{0};

    /// @brief Whether struct literals are allowed in the current expression context.
    /// @details Struct literals (`TypeName { field = val }`) are only permitted
    /// in initializer positions (var x = ..., return ..., rhs of assignment) to
    /// avoid ambiguity with block statements in for/if/while conditions.
    bool allowStructLiterals_{false};

    /// @}
};

} // namespace il::frontends::zia
