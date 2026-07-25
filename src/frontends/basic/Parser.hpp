//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares the Parser class, which performs syntax analysis of BASIC
// source code and constructs an Abstract Syntax Tree (AST).
//
// The parser is the second stage of the BASIC frontend compilation pipeline:
//   Lexer → Parser → AST → Semantic → Lowerer → IL
//
// Key Responsibilities:
// - Consumes tokens from the lexer and builds a structured AST representation
// - Parses BASIC language constructs including:
//   * Declarations (DIM for variables/arrays, SUB/FUNCTION definitions)
//   * Statements (assignments, control flow, I/O operations)
//   * Expressions (arithmetic, logical, string operations, function calls)
// - Produces a Program node containing:
//   * Main statement sequence (top-level executable code)
//   * Procedure definitions (SUB/FUNCTION bodies)
//   * Global declarations (shared variables, module-level state)
// - Performs syntax validation and reports parse errors via DiagnosticEmitter
// - Maintains lookahead buffer for efficient recursive descent parsing
//
// Design Notes:
// - Implements recursive descent parsing with operator precedence for expressions
// - The parser owns its lexer and manages the token stream internally
// - AST nodes are heap-allocated and returned via std::unique_ptr for ownership
//   transfer to the semantic analyzer
// - Error recovery: The parser attempts to synchronize on statement boundaries
//   (newlines, keywords) to report multiple errors in a single pass
// - Statement sequencing: Handles both line-number-based and modern structured
//   BASIC code, properly sequencing statements across line boundaries
//
// BASIC Language Features Supported:
// - Variables with type suffixes (%, &, !, #, $)
// - Multi-dimensional arrays with optional explicit bounds
// - Procedures (SUB) and functions (FUNCTION) with parameters and return values
// - Control flow: IF/THEN/ELSE, FOR/NEXT, DO/LOOP, WHILE/WEND, SELECT CASE
// - I/O: PRINT, INPUT, READ/DATA, file operations
// - Built-in functions: mathematical, string manipulation, type conversion
//
// Usage:
//   Parser parser(sourceText, fileId, &diagnosticEmitter);
//   auto program = parser.parseProgram();
//   if (program) {
//     // Proceed to semantic analysis
//   }
//
//===----------------------------------------------------------------------===//
/// @file Parser.hpp
/// @brief Declares the recursive-descent parser for the BASIC frontend.
/// @details Parser owns its lexer, lookahead tokens, and in-progress symbol
///          registries, and transfers ownership of constructed AST nodes to the
///          returned Program. Source text and optional diagnostic, source
///          manager, and include-stack services are borrowed.

#pragma once

#include "frontends/basic/DiagnosticEmitter.hpp"
#include "frontends/basic/IdentifierUtil.hpp"
#include "frontends/basic/Lexer.hpp"
#include "frontends/basic/StatementSequencer.hpp"
#include "frontends/basic/ast/DeclNodes.hpp"
#include "support/diag_expected.hpp"
#include "support/source_manager.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace il::frontends::basic {

/**
 * @brief Recursive descent parser for BASIC source code.
 *
 * Transforms BASIC source text into an abstract syntax tree (AST).
 * Supports structured programming constructs, OOP, and file inclusion.
 *
 * @invariant Source text must remain valid throughout parsing.
 * @invariant Diagnostics use the configured emitter when present and otherwise
 *            fall back to standard error.
 */
class Parser {
  public:
    /**
     * @brief Construct a parser for BASIC source text.
     *
     * @param src Source code to parse. Must remain valid during parsing.
     * @param file_id File identifier for diagnostic reporting.
     * @param emitter Diagnostic emitter for errors and warnings (optional, not owned).
     * @param sm Source manager for ADDFILE support (optional, not owned).
     * @param includeStack Stack for detecting circular includes (optional, not owned).
     * @param suppressUndefinedLabelCheck Skip undefined label validation (for included files).
     */
    Parser(std::string_view src,
           uint32_t file_id,
           DiagnosticEmitter *emitter = nullptr,
           il::support::SourceManager *sm = nullptr,
           std::vector<std::string> *includeStack = nullptr,
           bool suppressUndefinedLabelCheck = false);

    /**
     * @brief Parse the entire BASIC program into an AST.
     *
     * Main entry point that parses declarations, statements, and procedures.
     * Collects diagnostics and validates label references.
     *
     * @return Program AST on success, nullptr if parsing fails.
     * @post Diagnostics are emitted for any syntax errors encountered.
     */
    std::unique_ptr<Program> parseProgram();

  private:
    /// Expected-like result used by parser helpers that return structured errors.
    template <class T> using ErrorOr = il::support::Expected<T>;
    /// Tri-state statement result: no match, matched error, or owned statement.
    using StmtResult = std::optional<StmtPtr>;

    friend class StatementSequencer;

    /// @brief Create a statement sequencer bound to this parser instance.
    /// @return StatementSequencer referencing the parser's token stream.
    StatementSequencer statementSequencer();

    /// @brief Consume optional line labels that follow a line break.
    /// @param ctx Statement sequencer providing newline skipping helpers.
    /// @param followerKinds When non-empty, only consume the label when the
    ///        subsequent token is one of the specified kinds.
    void skipOptionalLineLabelAfterBreak(StatementSequencer &ctx,
                                         std::initializer_list<TokenKind> followerKinds = {});

    /// @brief Parse the body of a single IF-related branch.
    /// @param line Line number propagated to the branch statement.
    /// @param ctx Statement sequencer for separator management.
    /// @return Parsed statement belonging to the branch body.
    StmtPtr parseIfBranchBody(int line, StatementSequencer &ctx);

    mutable Lexer lexer_;                    ///< Provides tokens from the source buffer.
    mutable std::vector<Token> tokens_;      ///< Lookahead token buffer.
    size_t tokenStart_ = 0;                  ///< First unconsumed token in @ref tokens_.
    DiagnosticEmitter *emitter_ = nullptr;   ///< Diagnostic sink; not owned.
    std::unordered_set<std::string> arrays_; ///< Names of arrays declared via DIM.
    /// Namespace heads seen while parsing, used to distinguish qualified calls
    /// such as `A.F()` from object method calls such as `C.M()`.
    std::unordered_set<std::string> knownNamespaces_{};
    std::unordered_set<std::string> knownProcedures_; ///< Procedure identifiers seen so far.
    std::unordered_set<int> usedLabelNumbers_;        ///< Numeric labels already assigned.

    /// Integer constants tracked for SELECT CASE label resolution.
    /// Keys are canonicalized for case-insensitive lookup.
    std::unordered_map<std::string, int64_t> knownConstInts_{};
    /// String constants tracked for SELECT CASE label resolution.
    /// Keys are canonicalized for case-insensitive lookup.
    std::unordered_map<std::string, std::string> knownConstStrs_{};

    /// @brief Definition and first-reference state for one named BASIC label.
    struct NamedLabelEntry {
        int number = 0;                       ///< Synthesised numeric identifier for the label.
        bool defined = false;                 ///< True once the label definition has been seen.
        il::support::SourceLoc definitionLoc; ///< Location of the defining identifier.
        bool referenced = false;              ///< True when the label was referenced in source.
        il::support::SourceLoc referenceLoc;  ///< First location the label was referenced.
    };

    /// @brief Allocate the next synthetic number not present in the used-label set.
    /// @return Newly reserved candidate number.
    int allocateSyntheticLabelNumber();
    /// @brief Return the existing number for @p name or assign a synthetic one.
    /// @param name Named BASIC label, compared through canonical label keys.
    /// @return Stable numeric representation of @p name.
    int ensureLabelNumber(const std::string &name);
    /// @brief Test whether @p name has a registry entry.
    /// @param name Named BASIC label, compared through canonical label keys.
    /// @return `true` for either a definition or a forward-reference entry.
    bool hasLabelName(const std::string &name) const;
    /// @brief Find the numeric representation assigned to @p name.
    /// @param name Named BASIC label, compared through canonical label keys.
    /// @return Assigned number, or `std::nullopt` when no entry exists.
    std::optional<int> lookupLabelNumber(const std::string &name) const;
    /// @brief Record the source definition of a named label.
    /// @param tok Label token providing spelling and definition location.
    /// @param labelNumber Numeric representation already assigned to the label.
    void noteNamedLabelDefinition(const Token &tok, int labelNumber);
    /// @brief Record a named-label reference, preserving its first location.
    /// @param tok Label token providing spelling and reference location.
    /// @param labelNumber Numeric representation already assigned to the label.
    void noteNamedLabelReference(const Token &tok, int labelNumber);
    /// @brief Reserve a numeric label against future synthetic allocation.
    /// @param labelNumber User-written definition or branch-target number.
    void noteNumericLabelUsage(int labelNumber);

    std::unordered_map<std::string, NamedLabelEntry>
        namedLabels_;                    ///< Mapping from label names to ids.
    int nextSyntheticLabel_ = 1'000'000; ///< Next synthesised label id candidate.

    /// @brief Nesting depth of procedure bodies currently being parsed.
    /// @details Incremented while collecting statements for FUNCTION/SUB and OOP
    ///          member bodies so statement parsers can reject procedure-scoped
    ///          constructs such as USING.
    int procDepth_ = 0;
    /// @brief Nesting depth of active NAMESPACE declarations.
    /// @details Used to forbid USING directives inside namespaces per Phase 2 rules.
    int nsDepth_ = 0;

    /// @brief Current expression nesting depth for recursion guard.
    unsigned exprDepth_{0};
    /// @brief Maximum allowed expression nesting depth.
    static constexpr unsigned kMaxExprDepth = 512;

    /// @brief Current statement nesting depth for recursion guard.
    unsigned stmtDepth_{0};
    /// @brief Maximum allowed statement nesting depth.
    static constexpr unsigned kMaxStmtDepth = 512;

    /// @brief Current class being parsed (nullptr when not in a class).
    /// @details Used to detect intra-class method calls and rewrite them as method
    ///          calls on ME. Set when parsing class members; nullptr elsewhere.
    ClassDecl *currentClass_ = nullptr;

    /// @brief Registry that maps statement-leading tokens to parser callbacks.
    class StatementParserRegistry {
      public:
        /// Parser callback for a statement whose line number is not forwarded.
        using NoArgHandler = StmtPtr (Parser::*)();
        /// Parser callback receiving the statement's associated line number.
        using WithLineHandler = StmtPtr (Parser::*)(int);

        /// @brief Register handler without an explicit line parameter.
        /// @param kind Statement-leading token used as the table index.
        /// @param handler Callback stored in the no-line slot.
        void registerHandler(TokenKind kind, NoArgHandler handler);

        /// @brief Register handler that requires the originating line number.
        /// @param kind Statement-leading token used as the table index.
        /// @param handler Callback stored in the line-aware slot.
        void registerHandler(TokenKind kind, WithLineHandler handler);

        /// @brief Lookup registered handler for @p kind.
        /// @param kind Token kind to use as the table index.
        /// @return Callback pair, or two null pointers when @p kind is out of range.
        [[nodiscard]] std::pair<NoArgHandler, WithLineHandler> lookup(TokenKind kind) const;

        /// @brief Check whether @p kind begins a statement according to the registry.
        /// @param kind Token kind to query.
        /// @return `true` when either callback slot is populated.
        [[nodiscard]] bool contains(TokenKind kind) const;

      private:
        /// Fixed callback table indexed by the numeric TokenKind value.
        std::array<std::pair<NoArgHandler, WithLineHandler>,
                   static_cast<std::size_t>(TokenKind::Count)>
            entries_{};
    };

    /// @brief Access the lazily initialized process-wide statement registry.
    /// @return Const reference to the immutable populated registry.
    static const StatementParserRegistry &statementRegistry();
    /// @brief Construct a registry containing every parser category.
    /// @return Newly populated registry.
    static StatementParserRegistry buildStatementRegistry();
    /// @brief Add structured control-flow statement callbacks.
    /// @param registry Registry receiving callback entries.
    static void registerControlFlowParsers(StatementParserRegistry &registry);
    /// @brief Add runtime-oriented statement callbacks.
    /// @param registry Registry receiving callback entries.
    static void registerRuntimeParsers(StatementParserRegistry &registry);
    /// @brief Add console and file I/O statement callbacks.
    /// @param registry Registry receiving callback entries.
    static void registerIoParsers(StatementParserRegistry &registry);
    /// @brief Add core declaration, assignment, and procedure callbacks.
    /// @param registry Registry receiving callback entries.
    static void registerCoreParsers(StatementParserRegistry &registry);
    /// @brief Add object-oriented declaration and lifetime callbacks.
    /// @param registry Registry receiving callback entries.
    static void registerOopParsers(StatementParserRegistry &registry);

    /// @brief Result of processing an ADDFILE include.
    struct AddFileResult {
        bool success{false};                                    ///< True if include succeeded.
        std::unique_ptr<Program> subprog;                       ///< Parsed program from include.
        std::unordered_set<std::string> arrays;                 ///< Arrays from child parser.
        std::unordered_set<std::string> namespaces;             ///< Namespaces from child parser.
        std::unordered_map<std::string, int64_t> constInts;     ///< CONSTs from child.
        std::unordered_map<std::string, std::string> constStrs; ///< CONSTs from child.
        std::unordered_map<std::string, NamedLabelEntry> namedLabels; ///< Child named labels.
        std::unordered_set<int> usedLabelNumbers; ///< Label ids consumed while parsing child.
        int nextSyntheticLabel = 1'000'000;       ///< Child parser's next synthetic label id.
    };

    /// @brief Common logic for processing an ADDFILE directive.
    /// @details Handles path resolution, include depth/cycle checking, file reading,
    ///          child parser setup, and parsing. The returned state is merged by
    ///          the caller appropriate to its destination context.
    /// @param kw The ADDFILE keyword token.
    /// @return Result containing parsed subprogram and child parser state on success.
    AddFileResult processAddFileInclude(const Token &kw);

    /// @brief Handle a top-level ADDFILE directive if present.
    /// @details When the current token is ADDFILE and a source manager is available,
    ///          resolves, loads, and parses the included file, then merges its AST
    ///          into @p prog.
    /// @param prog Program receiving included declarations and main statements.
    /// @return True when a directive was handled (successfully or with diagnostics);
    ///         false when no ADDFILE directive was present.
    bool handleTopLevelAddFile(Program &prog);
    /// @brief Handle an ADDFILE directive in the current statement context.
    /// @details When the next token is ADDFILE and a source manager is available,
    ///          reads and parses the included file and appends its statements and
    ///          procedure declarations to @p dst.
    /// @param dst Owning statement vector receiving all included AST nodes.
    /// @return True when a directive was handled (successfully or with diagnostics);
    ///         false when no ADDFILE directive was present.
    bool handleAddFileInto(std::vector<StmtPtr> &dst);
    /// @brief Parse a CLASS declaration including fields and members.
    /// @return Owned class declaration, or null after an unrecoverable syntax error.
    StmtPtr parseClassDecl();
    /// @brief Parse the leading field-declaration section of a CLASS body.
    /// @param decl Class declaration receiving parsed fields.
    /// @param curAccess Carries the pending PUBLIC/PRIVATE prefix; on return it
    ///        holds any access prefix consumed just before the first non-field
    ///        member (which the member section then applies).
    void parseClassFieldSection(ClassDecl &decl, std::optional<Access> &curAccess);
    /// @brief Parse the method/property/destructor section of a CLASS body.
    /// @param decl Class declaration receiving parsed members.
    /// @param curAccess Pending explicit access modifier, if one was consumed.
    void parseClassMemberSection(ClassDecl &decl, std::optional<Access> curAccess);
    /// @brief Parse an INTERFACE declaration and its method signatures.
    /// @return Owned interface declaration, or null after an unrecoverable syntax error.
    StmtPtr parseInterfaceDecl();
    /// @brief Parse a user-defined TYPE declaration.
    /// @return Owned type declaration, or null after an unrecoverable syntax error.
    StmtPtr parseTypeDecl();
    /// @brief Parse an ENUM declaration and its members.
    /// @return Owned enum declaration, or null after an unrecoverable syntax error.
    StmtPtr parseEnumDecl();
    /// @brief Parse a DELETE statement for an object expression.
    /// @return Owned delete statement, or null after a syntax error.
    StmtPtr parseDeleteStatement();
    /// @brief Parse a TRY/CATCH statement.
    /// @return Newly constructed TryCatchStmt or nullptr on error.
    StmtPtr parseTryCatchStatement();

#include "frontends/basic/Parser_Token.hpp"

    /// @brief Parse a single BASIC statement at the given line number.
    /// @param line Line number associated with the statement.
    /// @return Parsed statement or nullptr on error.
    StmtPtr parseStatement(int line);

    /// @brief Attempt to parse a statement registered in the dispatcher.
    /// @param line Source line attached to the current statement.
    /// @return Optional statement; disengaged when no handler matches.
    StmtResult parseRegisteredStatement(int line);

    /// @brief Parse an assignment statement without an explicit LET keyword.
    /// @return Parsed statement or empty optional when the pattern does not match.
    StmtResult parseImplicitLet();

    /// @brief Parse a procedure call statement starting with an identifier.
    /// @param line Line metadata attached to the resulting statement.
    /// @return Parsed statement, null statement on error, or empty optional when
    ///         the pattern does not match.
    StmtResult parseCall(int line);

    /// @brief Diagnose and consume unexpected leading line numbers.
    /// @return Optional containing nullptr when an error was handled;
    ///         disengaged when the current token is not a line number.
    StmtResult parseLeadingLineNumberError();

    /// @brief Emit diagnostic for an unexpected leading line number token.
    /// @param tok Number token that introduced the error.
    void reportUnexpectedLineNumber(const Token &tok);

    /// @brief Emit diagnostic when a procedure call omits its opening parenthesis.
    /// @param identTok Identifier token introducing the call.
    /// @param nextTok Token following the identifier.
    void reportMissingCallParenthesis(const Token &identTok, const Token &nextTok);

    /// @brief Emit diagnostic when an identifier cannot be interpreted as a statement.
    /// @param identTok Identifier token responsible for the error.
    void reportInvalidCallExpression(const Token &identTok);

    /// @brief Emit diagnostic for an unknown statement introducer.
    /// @param tok Token that failed to match a known statement form.
    void reportUnknownStatement(const Token &tok);

    /// @brief Skip tokens after an error until reaching a boundary token.
    void resyncAfterError();

    /// @brief Identify whether the lookahead token begins a new statement.
    /// @param kind Token kind to classify.
    /// @return True when a handler or structural keyword marks the start of a new statement.
    bool isStatementStart(TokenKind kind) const;

    /// @brief Detect whether the current token sequence begins an implicit assignment.
    /// @return True when the current statement starts with an lvalue followed by '='.
    bool isImplicitAssignmentStart() const;

    /// @brief Parse a PRINT statement.
    /// @return PRINT statement node.
    StmtPtr parsePrintStatement();

    /// @brief Parse a WRITE # statement.
    /// @return WRITE statement node targeting a file channel.
    StmtPtr parseWriteStatement();

    /// @brief Parse a LET assignment statement.
    /// @return LET statement node.
    StmtPtr parseLetStatement();

    /// @brief Parse a CONST constant declaration statement.
    /// @return CONST statement node.
    StmtPtr parseConstStatement();

    /// @brief Parse the left-hand side of a LET assignment.
    /// @return Parsed lvalue expression.
    ExprPtr parseLetTarget();

    /// @brief Parse an IF statement starting at @p line.
    /// @param line Line number of the IF keyword.
    /// @return IF statement node.
    StmtPtr parseIfStatement(int line);

    /// @brief Parse a WHILE loop.
    /// @return WHILE statement node.
    StmtPtr parseWhileStatement();

    /// @brief Parse a SELECT CASE statement with optional CASE ELSE.
    /// @return SELECT CASE statement node.
    StmtPtr parseSelectCaseStatement();

    /// @brief Status information returned by CASE parsing helpers.
    struct SelectHandlerResult {
        bool handled = false;           ///< True when helper consumed tokens.
        bool emittedDiagnostic = false; ///< True when helper reported errors.
    };

    /// @brief Aggregates CASE body collection results.
    struct SelectBodyResult {
        std::vector<StmtPtr> body;                     ///< Statements collected.
        StatementSequencer::TerminatorInfo terminator; ///< Terminator metadata.
        bool emittedDiagnostic = false;                ///< Diagnostics emitted.
    };

    /// @brief Captures inline CASE body statements gathered after a colon.
    struct SelectInlineBodyResult {
        std::vector<StmtPtr> body; ///< Statements parsed on the same source line.
        Token terminator;          ///< End-of-line token that closed the inline body.
    };

    /// Diagnostic callback used by SELECT helpers without exposing parser internals.
    using SelectDiagnoseFn =
        std::function<void(il::support::SourceLoc, uint32_t, std::string_view, std::string_view)>;

    /// @brief Mutable state shared across SELECT header and arm phases.
    struct SelectParseState {
        /// SELECT node under construction.
        std::unique_ptr<SelectCaseStmt> stmt;
        /// Callback for SELECT-specific diagnostics.
        SelectDiagnoseFn diagnose;
        /// Location of the opening SELECT token.
        il::support::SourceLoc selectLoc;
        /// Whether a non-ELSE CASE was parsed.
        bool sawCaseArm = false;
        /// Whether CASE ELSE was parsed.
        bool sawCaseElse = false;
        /// Whether END SELECT remains required.
        bool expectEndSelect = true;
    };

    /// @brief Directs the SELECT arm loop after inspecting one directive.
    enum class SelectDispatchAction {
        /// No recognized directive was consumed.
        None,
        /// A directive was handled; parse the next arm.
        Continue,
        /// SELECT parsing reached its terminator.
        Terminate,
    };

    /// @brief Parse the opening SELECT CASE expression and initialize shared state.
    /// @return State containing the partial statement and diagnostic callback.
    SelectParseState parseSelectHeader();
    /// @brief Parse CASE directives and bodies into @p state.
    /// @param state SELECT statement and tracking flags to update.
    void parseSelectArms(SelectParseState &state);
    /// @brief Parse CASE ELSE when present at the current token.
    /// @param state SELECT statement and tracking flags to update.
    /// @return `true` when CASE ELSE was recognized and consumed.
    bool parseSelectElse(SelectParseState &state);
    /// @brief Handle an END SELECT or other SELECT-level directive.
    /// @param state SELECT statement and tracking flags to update.
    /// @return Action directing the enclosing arm loop.
    SelectDispatchAction dispatchSelectDirective(SelectParseState &state);

    /// @brief Collect a CASE/CASE ELSE body until the next arm or END SELECT.
    /// @return Aggregated statements and terminator metadata.
    SelectBodyResult collectSelectBody();

    /// @brief Parse colon-terminated statements that immediately follow a CASE header.
    /// @return Statements parsed before the end-of-line terminator.
    SelectInlineBodyResult collectInlineSelectBody();

    /// @brief Handle END SELECT terminator encountered while parsing.
    /// @param stmt Statement under construction whose range gets extended.
    /// @param sawCaseArm Whether a CASE arm has been parsed so far.
    /// @param expectEndSelect Flag tracking whether END SELECT is still required.
    /// @param diagnose Diagnostic callback mirroring parser emission.
    /// @return Helper status describing token consumption and diagnostics.
    SelectHandlerResult handleEndSelect(SelectCaseStmt &stmt,
                                        bool sawCaseArm,
                                        bool &expectEndSelect,
                                        const SelectDiagnoseFn &diagnose);

    /// @brief Parse CASE ELSE arm when encountered at the current position.
    /// @param stmt Statement receiving the CASE ELSE body.
    /// @param sawCaseArm Whether at least one CASE arm preceded the ELSE.
    /// @param sawCaseElse Tracks whether a CASE ELSE has already appeared.
    /// @param diagnose Diagnostic callback mirroring parser emission.
    /// @return Helper status describing token consumption and diagnostics.
    SelectHandlerResult consumeCaseElse(SelectCaseStmt &stmt,
                                        bool sawCaseArm,
                                        bool &sawCaseElse,
                                        const SelectDiagnoseFn &diagnose);

    /// Token-buffer cursor used to parse CASE label syntax transactionally.
    struct Cursor;
    /// Syntax-only CASE arm representation produced before semantic lowering.
    struct CaseArmSyntax;

    /// @brief Parse CASE label syntax through a transactional cursor.
    /// @param cursor Cursor advanced over the CASE label tokens.
    /// @return Syntax representation or a structured parse error.
    il::support::Expected<CaseArmSyntax> parseCaseArmSyntax(Cursor &cursor);
    /// @brief Convert syntax-only CASE labels into the AST representation.
    /// @param syntax Successfully parsed CASE label syntax.
    /// @return Lowered CASE arm or a structured conversion error.
    il::support::Expected<CaseArm> lowerCaseArm(const CaseArmSyntax &syntax);
    /// @brief Validate the semantic constraints of a lowered CASE arm.
    /// @param arm CASE arm to validate.
    /// @return Success or a structured validation error.
    ErrorOr<void> validateCaseArm(const CaseArm &arm);

    /// @brief Parse a CASE arm including label list and statement body.
    /// @return Parsed CASE arm.
    CaseArm parseCaseArm();

    /// @brief Parse the CASE ELSE body until END SELECT.
    /// @return Statements contained within CASE ELSE and the location of the
    ///         terminating end-of-line.
    std::pair<std::vector<StmtPtr>, il::support::SourceLoc> parseCaseElseBody();

    /// @brief Mutable state shared by IF header, block, and ELSE-chain phases.
    struct IfParseState {
        /// IF node under construction.
        std::unique_ptr<IfStmt> stmt;
        /// Source line associated with the opening IF.
        int line = 0;
        /// Location of the opening IF token.
        il::support::SourceLoc loc;
    };

    /// @brief Parse the IF condition and initialize a statement node.
    /// @param line Source line associated with the opening IF.
    /// @return State used by block and ELSE-chain parsing.
    IfParseState parseIfHeader(int line);
    /// @brief Parse the primary THEN body into @p state.
    /// @param state IF node and source metadata to update.
    void parseIfBlock(IfParseState &state);
    /// @brief Parse ELSEIF and ELSE branches into @p state.
    /// @param state IF node and source metadata to update.
    void parseElseChain(IfParseState &state);

    /// @brief Parse a DO ... LOOP statement.
    /// @return DO statement node with optional tests.
    StmtPtr parseDoStatement();

    /// @brief Parse a FOR loop.
    /// @return FOR statement node.
    StmtPtr parseForStatement();

    /// @brief Parse a NEXT statement closing a loop.
    /// @return NEXT statement node.
    StmtPtr parseNextStatement();

    /// @brief Parse an EXIT statement identifying the loop kind.
    /// @return EXIT statement node.
    StmtPtr parseExitStatement();

    /// @brief Parse a GOTO statement.
    /// @return GOTO statement node.
    StmtPtr parseGotoStatement();

    /// @brief Parse a GOSUB statement.
    /// @return GOSUB statement node.
    StmtPtr parseGosubStatement();

    /// @brief Parse an OPEN statement configuring file I/O.
    /// @return OPEN statement node.
    StmtPtr parseOpenStatement();

    /// @brief Parse a CLOSE statement releasing a channel.
    /// @return CLOSE statement node.
    StmtPtr parseCloseStatement();

    /// @brief Parse a SEEK statement repositioning a channel.
    /// @return SEEK statement node.
    StmtPtr parseSeekStatement();

    /// @brief Parse an ON ERROR GOTO statement.
    /// @return ON ERROR statement node.
    StmtPtr parseOnErrorGotoStatement();

    /// @brief Parse an END statement.
    /// @return END statement node.
    StmtPtr parseEndStatement();

    /// @brief Parse an INPUT statement.
    /// @return INPUT statement node.
    StmtPtr parseInputStatement();

    /// @brief Parse a LINE INPUT # statement.
    /// @return LINE INPUT statement node.
    StmtPtr parseLineInputStatement();

    /// @brief Parse a RESUME statement.
    /// @return RESUME statement node.
    StmtPtr parseResumeStatement();

    /// @brief Parse a DIM statement defining arrays.
    /// @return DIM statement node.
    StmtPtr parseDimStatement();

    /// @brief Parse a STATIC statement declaring persistent procedure-local variables.
    /// @return STATIC statement node.
    StmtPtr parseStaticStatement();
    /// @brief Parse a SHARED statement listing variables that map to module-level state.
    /// @return SHARED declaration statement node.
    StmtPtr parseSharedStatement();

    /// @brief Parse a REDIM statement resizing arrays.
    /// @return REDIM statement node.
    StmtPtr parseReDimStatement();

    /// @brief Parse a RANDOMIZE statement.
    /// @return RANDOMIZE statement node.
    StmtPtr parseRandomizeStatement();

    /// @brief Parse a SWAP statement.
    /// @return SWAP statement node.
    StmtPtr parseSwapStatement();

    /// @brief Parse a BEEP statement that emits a beep/bell sound.
    /// @return BEEP statement node.
    StmtPtr parseBeepStatement();

    /// @brief Parse a CLS statement clearing the display.
    /// @return CLS statement node.
    StmtPtr parseClsStatement();

    /// @brief Parse a COLOR statement adjusting the palette.
    /// @return COLOR statement node.
    StmtPtr parseColorStatement();

    /// @brief Parse a LOCATE statement moving the cursor.
    /// @return LOCATE statement node.
    StmtPtr parseLocateStatement();

    /// @brief Parse a CURSOR statement controlling cursor visibility.
    /// @return CURSOR statement node.
    StmtPtr parseCursorStatement();

    /// @brief Parse an ALTSCREEN statement controlling alternate screen buffer.
    /// @return ALTSCREEN statement node.
    StmtPtr parseAltScreenStatement();

    /// @brief Parse a SLEEP statement blocking for milliseconds.
    /// @return SLEEP statement node.
    StmtPtr parseSleepStatement();

    /// @brief Parse a FUNCTION definition including body.
    /// @return FUNCTION statement node.
    StmtPtr parseFunctionStatement();

    /// @brief Parse the header of a FUNCTION without its body.
    /// @return Newly allocated function declaration.
    std::unique_ptr<FunctionDecl> parseFunctionHeader();

    /// @brief Parse the body of a function and attach statements to @p fn.
    /// @param fn Function declaration to populate.
    void parseFunctionBody(FunctionDecl *fn);

    /// @brief Parse a sequence of statements for a procedure-like declaration.
    /// @param endKind Token that must follow END to terminate the body.
    /// @param body Destination vector receiving parsed statements.
    /// @return Location of the END keyword; invalid if the keyword is absent.
    il::support::SourceLoc parseProcedureBody(TokenKind endKind, std::vector<StmtPtr> &body);

    /// @brief Remember a procedure name for later diagnostics.
    /// @param name BASIC identifier of the procedure.
    void noteProcedureName(std::string_view name);

    /// @brief Check whether @p name has been seen as a procedure declaration.
    /// @param name Identifier to test.
    /// @return True when @p name is a known procedure.
    bool isKnownProcedureName(const std::string &name) const;

    /// @brief Pre-scan source for SUB/FUNCTION names to enable parenthesis-free calls.
    /// @details Performs a quick lexer scan to find all SUB/FUNCTION declarations
    ///          and registers their names before the main parse begins. Forward
    ///          references (calls before definition) require this pre-scan. (BUG-OOP-020)
    /// @param src Source text to scan.
    /// @param file_id File identifier for the lexer.
    void prescanProcedureNames(std::string_view src, uint32_t file_id);

    /// @brief Parse a SUB definition including body.
    /// @return SUB statement node.
    StmtPtr parseSubStatement();

    /// @brief Parse an EXPORT FUNCTION/SUB statement.
    /// @return Function or sub declaration with export linkage.
    StmtPtr parseExportStatement();

    /// @brief Parse a DECLARE FOREIGN FUNCTION/SUB statement (import, no body).
    /// @return Function or sub declaration with import linkage.
    StmtPtr parseDeclareStatement();

    /// @brief Parse a RETURN statement.
    /// @return RETURN statement node.
    StmtPtr parseReturnStatement();

    /// @brief Parse a NAMESPACE declaration with dotted path and body.
    /// @return NamespaceDecl node owning path segments and nested statements.
    StmtPtr parseNamespaceDecl();

    /// @brief Parse a USING directive with optional alias.
    /// @details Supports "USING Foo.Bar.Baz" and "USING FB = Foo.Bar".
    /// @return UsingDecl node capturing namespace path and alias.
    StmtPtr parseUsingDecl();

    /// @brief Parse a USING resource statement (USING x AS Type = expr ... END USING).
    /// @param loc Source location of the USING keyword (already consumed).
    /// @return UsingStmt node for resource management.
    StmtPtr parseUsingStatement(il::support::SourceLoc loc);

    /// @brief Parse a comma-separated parameter list inside parentheses.
    /// @return Vector of parsed parameters.
    std::vector<Param> parseParamList();

    /// @brief Determine BASIC type from identifier suffix.
    /// @param name Identifier possibly carrying a type suffix ($, %, &, !, #).
    /// @return Resolved type.
    Type typeFromSuffix(std::string_view name);

    /// @brief Parse a BASIC type keyword following AS.
    /// @return Resolved BASIC type, defaults to I64 on mismatch.
    Type parseTypeKeyword();

    /// @brief Parse an optional BASIC return type following a FUNCTION header.
    /// @return Parsed BASIC type; returns Unknown when no recognised annotation is present.
    BasicType parseBasicType();

    /// @brief Parse an expression using precedence climbing.
    /// @param min_prec Minimum precedence to enforce.
    /// @return Parsed expression node.
    ExprPtr parseExpression(int min_prec = 0);

    /// @brief Parse unary operators and delegate to primaries when absent.
    /// @return Parsed unary expression node.
    ExprPtr parseUnary();

    /// @brief Parse infix operators with precedence handling.
    /// @param min_prec Minimum precedence to enforce.
    /// @return Parsed expression node.
    ExprPtr parseBinary(int min_prec);

    /// @brief Parse a primary expression such as literals or parenthesized forms.
    /// @return Parsed primary expression node.
    ExprPtr parsePrimary();

    /// @brief Parse postfix member access or method invocation chains.
    /// @param expr Expression to extend.
    /// @return Expression with postfix operations applied.
    ExprPtr parsePostfix(ExprPtr expr);

    /// @brief Parse a NEW expression allocating a class instance.
    /// @return Newly allocated expression node.
    ExprPtr parseNewExpression();

    /// @brief Parse a dotted qualified identifier into segments.
    /// @return Pair of segments and starting SourceLoc; empty segments on failure.
    std::pair<std::vector<std::string>, il::support::SourceLoc> parseQualifiedIdentSegments();

    /// @brief Parse LBOUND/UBOUND intrinsics.
    /// @param keyword Token identifying which bound to read.
    /// @return Parsed intrinsic expression node.
    ExprPtr parseBoundIntrinsic(TokenKind keyword);

    /// @brief Parse LOF/EOF/LOC file channel intrinsics.
    /// @param keyword Token identifying the intrinsic.
    /// @return Parsed intrinsic expression node.
    ExprPtr parseChannelIntrinsic(TokenKind keyword);

    /// @brief Parse a numeric literal expression.
    /// @return Parsed number expression node.
    ExprPtr parseNumber();

    /// @brief Parse a string literal expression.
    /// @return Parsed string expression node.
    ExprPtr parseString();

    /// @brief Parse a call to a builtin function.
    /// @param builtin Which builtin is being invoked.
    /// @param loc Source location of the call.
    /// @return Parsed builtin call expression node.
    ExprPtr parseBuiltinCall(BuiltinCallExpr::Builtin builtin, il::support::SourceLoc loc);

    /// @brief Parse a reference to a variable.
    /// @param name Variable identifier.
    /// @param loc Source location of the identifier.
    /// @return Variable reference expression node.
    ExprPtr parseVariableRef(std::string_view name, il::support::SourceLoc loc);

    /// @brief Parse a reference to an array element.
    /// @param name Array identifier.
    /// @param loc Source location of the identifier.
    /// @return Array reference expression node.
    ExprPtr parseArrayRef(std::string_view name, il::support::SourceLoc loc);

    /// @brief Parse either an array or variable reference based on lookahead.
    /// @return Parsed reference expression node.
    ExprPtr parseArrayOrVar();

    /// @brief Return the precedence value for operator token @p k.
    /// @param k Operator token kind.
    /// @return Numeric precedence used by the expression parser.
    int precedence(TokenKind k);

    // ========================================================================
    // Error Reporting Helpers
    // ========================================================================

    /// @brief Emit a diagnostic error with code and token location.
    /// @param code Diagnostic code (e.g., "B0001").
    /// @param tok Token providing source location.
    /// @param message Error message text.
    void emitError(std::string_view code, const Token &tok, std::string message);

    /// @brief Emit a diagnostic error with code and explicit location.
    /// @param code Diagnostic code (e.g., "B0001").
    /// @param loc Source location.
    /// @param message Error message text.
    void emitError(std::string_view code, il::support::SourceLoc loc, std::string message);

    /// @brief Emit a diagnostic warning with code and token location.
    /// @param code Diagnostic code (e.g., "B9000").
    /// @param tok Token providing source location.
    /// @param message Warning message text.
    void emitWarning(std::string_view code, const Token &tok, std::string message);

    /// @brief Drop consumed tokens once the buffer prefix grows large enough.
    /// @details Keeps @ref consume amortized O(1) instead of erasing from the
    ///          front of the vector for every token.
    void compactConsumedTokens();

  private:
    /// @brief Common helper for diagnostic emission.
    /// @param sev Diagnostic severity.
    /// @param code Stable diagnostic identifier.
    /// @param loc Start of the highlighted source range.
    /// @param len Highlight length in bytes.
    /// @param message Human-readable diagnostic text.
    void emitDiagnostic(il::support::Severity sev,
                        std::string_view code,
                        il::support::SourceLoc loc,
                        uint32_t len,
                        std::string message);

    // ---------------------------------------------------------------------
    // ADDFILE support
    // ---------------------------------------------------------------------
    il::support::SourceManager *sm_ = nullptr;         ///< For path resolution and file ids.
    std::vector<std::string> *includeStack_ = nullptr; ///< Tracks include chain for cycles.
    bool suppressUndefinedNamedLabelCheck_ = false;    ///< Skip undefined label check when true.
    int maxIncludeDepth_ = 32;                         ///< Hard limit to prevent recursion.
};

} // namespace il::frontends::basic
