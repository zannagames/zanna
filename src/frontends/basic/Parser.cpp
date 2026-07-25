//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/Parser.cpp
// Purpose: Implement the façade that orchestrates BASIC parsing and routes work
//          to specialised statement and expression modules.
// Links: docs/tutorials/basic-tutorial.md#parser
//
//===----------------------------------------------------------------------===//

/// @file Parser.cpp
/// @brief Houses high-level orchestration logic for the BASIC parser.
/// @details Initialises the token buffer, constructs statement parser
///          registries, and exposes the top-level parse routine that produces
///          the AST.  Specific statement and expression production rules are
///          delegated to sibling translation units.

#include "frontends/basic/Parser.hpp"
#include "common/Filesystem.hpp"
#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/LineUtils.hpp"
#include "frontends/basic/Options.hpp"
#include "frontends/basic/StringUtils.hpp"
#include "support/source_manager.hpp"
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace il::frontends::basic {
namespace {

/// @brief Canonicalize a parser label key while preserving recovery for malformed names.
/// @details BASIC labels are case-insensitive. Valid identifier labels use
///          @ref CanonicalizeIdent; malformed recovery labels fall back to the
///          original spelling so diagnostics can still refer to the source text.
/// @param name Label spelling from a token or caller.
/// @return Stable key used by the parser's named-label table.
std::string canonicalLabelKey(std::string_view name) {
    std::string canon = CanonicalizeIdent(name);
    if (!canon.empty() || name.empty())
        return canon;
    return std::string(name);
}

} // namespace

/// @brief Construct a parser for the given source.
/// @details Instantiates the lexer, stores the diagnostic emitter pointer, and
///          seeds the token buffer with the first lookahead token.  The eager
///          priming simplifies the rest of the parser, which can assume at least
///          one token is buffered before processing productions.
/// @param src Full BASIC source to parse.
/// @param file_id Identifier for diagnostics.
/// @param emitter Optional borrowed destination for structured diagnostics.
/// @param sm Optional borrowed source manager required by `ADDFILE`.
/// @param includeStack Optional borrowed canonical-path stack for include-cycle checks.
/// @param suppressUndefinedLabelCheck Skip the final undefined named-label pass
///        when this parser represents an included file.
/// @pre @p src and every non-null borrowed pointer remain valid while parsing.
Parser::Parser(std::string_view src,
               uint32_t file_id,
               DiagnosticEmitter *emitter,
               il::support::SourceManager *sm,
               std::vector<std::string> *includeStack,
               bool suppressUndefinedLabelCheck)
    : lexer_(src, file_id), emitter_(emitter), sm_(sm), includeStack_(includeStack),
      suppressUndefinedNamedLabelCheck_(suppressUndefinedLabelCheck) {
    tokens_.push_back(lexer_.next());

    if (il::frontends::basic::FrontendOptions::enableRuntimeNamespaces()) {
        knownNamespaces_.insert("zanna");
    }

    // Pre-scan source for SUB/FUNCTION names to enable parenthesis-free calls.
    // Forward references (calls before definition) require this pre-scan. (BUG-OOP-020)
    prescanProcedureNames(src, file_id);
}

/// @brief Pre-register SUB and FUNCTION names for call disambiguation.
/// @details Runs an independent lexer over the complete source so calls that
///          precede their declarations can be recognized without parentheses.
///          Each following identifier has its type suffix stripped and is
///          canonicalized when possible before insertion.
/// @param src Complete source text scanned independently of the parser token buffer.
/// @param file_id Source identifier assigned to tokens produced by the scan.
/// @pre @p src remains valid for the duration of this scan.
void Parser::prescanProcedureNames(std::string_view src, uint32_t file_id) {
    Lexer scanner(src, file_id);
    Token tok = scanner.next();
    while (tok.kind != TokenKind::EndOfFile) {
        // Look for SUB <ident> or FUNCTION <ident>
        if (tok.kind == TokenKind::KeywordSub || tok.kind == TokenKind::KeywordFunction) {
            Token next = scanner.next();
            if (next.kind == TokenKind::Identifier) {
                // Register the procedure name for parenthesis-free call detection.
                std::string canon = CanonicalizeIdent(StripTypeSuffix(next.lexeme));
                knownProcedures_.insert(canon.empty() ? next.lexeme : std::move(canon));
            }
            tok = next;
        } else {
            tok = scanner.next();
        }
    }
}

/// @brief Create a statement sequencer bound to this parser instance.
/// @details Returns a lightweight helper that coordinates optional line numbers
///          and statement separators before delegating to parser callbacks.
///          Exposing the sequencer by value keeps the API ergonomic while still
///          encapsulating token-buffer access.
/// @return Sequencer that shares access to the parser's token stream.
StatementSequencer Parser::statementSequencer() {
    return StatementSequencer(*this);
}

/// @brief Allocate a unique synthetic label number not in use.
/// @details Increments the synthetic label counter until finding an unused number,
///          ensuring no collision with user-defined numeric labels.  Used during
///          parsing to generate internal labels for unlabeled BASIC statements.
/// @return Newly allocated label number guaranteed not to conflict with any used labels.
int Parser::allocateSyntheticLabelNumber() {
    while (usedLabelNumbers_.contains(nextSyntheticLabel_))
        ++nextSyntheticLabel_;
    return nextSyntheticLabel_++;
}

/// @brief Ensure a named label has an assigned number, creating one if needed.
/// @details Looks up the label name in the registry. If found, returns its existing
///          number. Otherwise allocates a fresh synthetic number, registers it, and
///          marks it as used to prevent collisions.  This allows forward references
///          to labels that haven't been defined yet.
/// @param name Label identifier from BASIC source.
/// @return The label number (existing or newly allocated).
int Parser::ensureLabelNumber(const std::string &name) {
    const std::string key = canonicalLabelKey(name);
    auto it = namedLabels_.find(key);
    if (it != namedLabels_.end())
        return it->second.number;

    int labelNumber = allocateSyntheticLabelNumber();
    NamedLabelEntry entry{};
    entry.number = labelNumber;
    namedLabels_.emplace(key, entry);
    usedLabelNumbers_.insert(labelNumber);
    return labelNumber;
}

/// @brief Check whether a named label exists in the registry.
/// @details Queries the parser's label registry without modifying it.  Returns
///          true if the label has been registered (either via definition or
///          reference), false otherwise.
/// @param name Label identifier to query.
/// @return True if the label has been registered; false if unknown.
bool Parser::hasLabelName(const std::string &name) const {
    return namedLabels_.find(canonicalLabelKey(name)) != namedLabels_.end();
}

/// @brief Look up the label number assigned to a named label.
/// @details Searches the registry for the given label name.  If found, returns
///          the associated label number.  Otherwise returns std::nullopt, allowing
///          callers to distinguish between undefined labels and valid entries.
/// @param name Label identifier to look up.
/// @return Label number if the label exists, std::nullopt otherwise.
std::optional<int> Parser::lookupLabelNumber(const std::string &name) const {
    auto it = namedLabels_.find(canonicalLabelKey(name));
    if (it == namedLabels_.end())
        return std::nullopt;
    return it->second.number;
}

/// @brief Record the definition of a named label at a specific location.
/// @details Marks the label as defined, records its source location, and adds it
///          to the used label set.  If the label was already defined, emits a
///          diagnostic error.  This enables detection of duplicate label definitions.
/// @param tok Token containing the label name and source location.
/// @param labelNumber The numeric label number associated with this named label.
void Parser::noteNamedLabelDefinition(const Token &tok, int labelNumber) {
    usedLabelNumbers_.insert(labelNumber);
    const std::string key = canonicalLabelKey(tok.lexeme);
    auto it = namedLabels_.find(key);
    if (it == namedLabels_.end()) {
        NamedLabelEntry entry{};
        entry.number = labelNumber;
        entry.defined = true;
        entry.definitionLoc = tok.loc;
        namedLabels_.emplace(key, entry);
        return;
    }

    if (!it->second.defined) {
        it->second.defined = true;
        it->second.definitionLoc = tok.loc;
        return;
    }

    std::string msg = "label '" + tok.lexeme + "' already defined";
    /// @brief Emits error.
    emitError("B0001", tok, std::move(msg));
}

/// @brief Record a reference to a named label.
/// @details Marks the label as referenced and records the source location of the
///          reference.  If this is the first time the label is seen, creates a new
///          entry in the label registry.  This supports forward references where a
///          GOTO/GOSUB appears before the label definition.
/// @param tok Token containing the label name and reference location.
/// @param labelNumber The numeric label number associated with this named label.
void Parser::noteNamedLabelReference(const Token &tok, int labelNumber) {
    usedLabelNumbers_.insert(labelNumber);
    const std::string key = canonicalLabelKey(tok.lexeme);
    auto it = namedLabels_.find(key);
    if (it == namedLabels_.end()) {
        NamedLabelEntry entry{};
        entry.number = labelNumber;
        entry.referenced = true;
        entry.referenceLoc = tok.loc;
        namedLabels_.emplace(key, entry);
        return;
    }

    auto &entry = it->second;
    entry.referenced = true;
    if (!entry.referenceLoc.isValid())
        entry.referenceLoc = tok.loc;
}

/// @brief Record that a numeric label number is in use.
/// @details Adds the label number to the set of used labels to prevent synthetic
///          label allocation from colliding with user-defined numeric labels.  This
///          is called for both label definitions and GOTO/GOSUB targets.
/// @param labelNumber The numeric label being marked as used.
void Parser::noteNumericLabelUsage(int labelNumber) {
    usedLabelNumbers_.insert(labelNumber);
}

/// @brief Populate the registry of statement parsing callbacks.
/// @details Populates the registry with handlers grouped by category (core,
///          control-flow, runtime, and I/O).  The registry is returned by value
///          so callers can store it in static storage without exposing
///          construction details.
/// @return Registry containing core, control-flow, runtime, I/O, and OOP handlers.
Parser::StatementParserRegistry Parser::buildStatementRegistry() {
    StatementParserRegistry registry;
    registerCoreParsers(registry);
    registerControlFlowParsers(registry);
    /// @brief Handles error condition.
    registerRuntimeParsers(registry);
    registerIoParsers(registry);
    registerOopParsers(registry);
    return registry;
}

/// @brief Parse an optional BASIC type annotation for function return values.
///
/// @details Consumes recognised BASIC type names used in FUNCTION headers after an
///          `AS` clause.  The helper mirrors the accepted spellings from DIM so
///          semantic analysis observes consistent defaults.  When no known type is
///          present the current token remains untouched and callers see
///          BasicType::Unknown, allowing them to diagnose the omission if
///          necessary.
///
/// @return Parsed BASIC return type or @ref BasicType::Unknown when unrecognised.
BasicType Parser::parseBasicType() {
    const Token &tok = peek();

    if (tok.kind == TokenKind::KeywordBoolean) {
        consume();
        return BasicType::Bool;
    }

    if (tok.kind != TokenKind::Identifier)
        return BasicType::Unknown;

    std::string upper = string_utils::to_upper(tok.lexeme);
    if (upper == "INTEGER" || upper == "LONG" || upper == "INT") {
        consume();
        return BasicType::Int;
    }
    if (upper == "DOUBLE" || upper == "FLOAT" || upper == "SINGLE") {
        consume();
        return BasicType::Float;
    }
    if (upper == "STRING") {
        consume();
        return BasicType::String;
    }
    if (upper == "BOOLEAN" || upper == "BOOL") {
        consume();
        return BasicType::Bool;
    }
    return BasicType::Unknown;
}

/// @brief Access the singleton statement parser registry.
/// @details Initialises the registry on first use by delegating to
///          @ref buildStatementRegistry.  Subsequent calls reuse the static
///          instance, ensuring parser construction remains inexpensive.
const Parser::StatementParserRegistry &Parser::statementRegistry() {
    static StatementParserRegistry registry = buildStatementRegistry();
    return registry;
}

/// @brief Install a statement handler that does not expect an explicit line number.
/// @details Stores the callback in the registry entry associated with @p kind.
///          The registry distinguishes between handlers that consume prefixed
///          line numbers and those that do not.
/// @param kind Leading token kind used as the registry-table index.
/// @param handler Parser member-function callback installed in the no-line slot.
void Parser::StatementParserRegistry::registerHandler(TokenKind kind, NoArgHandler handler) {
    entries_[static_cast<std::size_t>(kind)].first = handler;
}

/// @brief Install a statement handler that receives the parsed line number.
/// @details Complements the no-argument registration to support statements that
///          require awareness of their source line during parsing.
/// @param kind Leading token kind used as the registry-table index.
/// @param handler Parser member-function callback installed in the line-aware slot.
void Parser::StatementParserRegistry::registerHandler(TokenKind kind, WithLineHandler handler) {
    entries_[static_cast<std::size_t>(kind)].second = handler;
}

std::pair<Parser::StatementParserRegistry::NoArgHandler,
          Parser::StatementParserRegistry::WithLineHandler>
/// @brief Retrieve registered handlers for the given token kind.
/// @details Returns the pair of callbacks for @p kind, substituting
///          `{nullptr, nullptr}` when the registry lacks an entry.  The helper
///          performs bounds checking to keep invalid token kinds from indexing
///          past the table.
/// @param kind Token kind whose no-line and line-aware callbacks are requested.
/// @return Registered callback pair, or two null pointers when @p kind is out of range.
Parser::StatementParserRegistry::lookup(TokenKind kind) const {
    const auto index = static_cast<std::size_t>(kind);
    if (index >= entries_.size())
        return {nullptr, nullptr};
    return entries_[index];
}

/// @brief Test whether any handler exists for the token kind.
/// @details Uses @ref lookup to fetch the callback pair and reports true when at
///          least one handler is registered.
/// @param kind Token kind to query.
/// @return `true` when either callback slot contains a handler.
bool Parser::StatementParserRegistry::contains(TokenKind kind) const {
    const auto [noArg, withLine] = lookup(kind);
    return noArg != nullptr || withLine != nullptr;
}

/// @brief Parse the entire BASIC program.
/// @details Iteratively consumes statement lines using the sequencer, routing
///          procedure declarations to @ref Program::procs while collecting other
///          statements into @ref Program::main.  Parsing continues until an
///          end-of-file token is encountered; empty lines are skipped.  The
///          program node inherits its source location from the first token to
///          aid diagnostics.
/// @return Root program node with separated procedure and main sections.
std::unique_ptr<Program> Parser::parseProgram() {
    auto prog = std::make_unique<Program>();
    prog->loc = peek().loc;
    auto seq = statementSequencer();
    while (!at(TokenKind::EndOfFile)) {
        seq.skipLineBreaks();
        if (at(TokenKind::EndOfFile))
            break;
        if (at(TokenKind::Number) && peek(1).kind == TokenKind::KeywordAddfile) {
            Token numberTok = consume();
            if (auto parsed = parseLineNumberLiteral(numberTok.lexeme))
                noteNumericLabelUsage(*parsed);
            else
                emitError("B0001", numberTok, "line number is out of range");
        }
        if (at(TokenKind::KeywordAddfile)) {
            if (handleTopLevelAddFile(*prog)) {
                // Either handled or diagnosed; continue to next line.
                continue;
            }
        }
        auto root = seq.parseStatementLine();
        if (!root)
            continue;
        if (is<FunctionDecl>(*root) || is<SubDecl>(*root)) {
            prog->procs.push_back(std::move(root));
        } else {
            prog->main.push_back(std::move(root));
        }
    }
    if (!suppressUndefinedNamedLabelCheck_) {
        bool hasUndefinedNamedLabel = false;
        for (const auto &[name, entry] : namedLabels_) {
            if (!entry.referenced || entry.defined)
                continue;
            hasUndefinedNamedLabel = true;
            std::string msg = "Undefined label: " + name;
            /// @brief Emits error.
            emitError("B0002", entry.referenceLoc, std::move(msg));
        }
        if (hasUndefinedNamedLabel)
            return nullptr;
    }
    return prog;
}

// -----------------------------------------------------------------------------
// ADDFILE handling
// -----------------------------------------------------------------------------

/// @brief Parse, load, and recursively parse one consumed `ADDFILE` directive.
/// @details Consumes the required string path and the rest of its source line,
///          resolves a relative path against the including file, enforces the
///          include-depth, cycle, 64-MiB, and source-ID limits, and parses the
///          included contents with undefined-label checking deferred to the
///          parent. On success, the returned value carries the child program
///          plus array, namespace, constant, and label state for merging.
/// @param kw Already-consumed `ADDFILE` token identifying the including file.
/// @return A result whose `success` flag is true only after the child program
///         and mergeable child state have been produced.
/// @pre The parser has non-null source-manager and diagnostic-emitter pointers.
Parser::AddFileResult Parser::processAddFileInclude(const Token &kw) {
    AddFileResult result;

    Token pathTok = expect(TokenKind::String);
    if (pathTok.kind != TokenKind::String)
        return result;
    std::string rawPath = pathTok.lexeme;

    while (!at(TokenKind::EndOfFile) && !at(TokenKind::EndOfLine))
        consume();
    if (at(TokenKind::EndOfLine))
        consume();

    // Resolve path relative to including file.
    const uint32_t includingFileId = kw.loc.file_id;
    std::filesystem::path base = zanna::filesystem::pathFromUtf8(sm_->getPath(includingFileId));
    std::filesystem::path candidate = zanna::filesystem::pathFromUtf8(rawPath);
    std::filesystem::path resolved =
        candidate.is_absolute() ? candidate : base.parent_path() / candidate;

    std::error_code ec;
    std::filesystem::path canon = std::filesystem::weakly_canonical(resolved, ec);
    const std::filesystem::path nativeCanonical = ec ? resolved.lexically_normal() : canon;
    const std::string canonStr = zanna::filesystem::pathToUtf8(nativeCanonical);

    // Check include depth and cycles.
    /// @brief Restores the caller-owned include stack on every return path.
    struct IncludeStackGuard {
        /// Borrowed stack from which the active include is removed.
        std::vector<std::string> *stack = nullptr;
        /// Whether this invocation pushed an entry that must be removed.
        bool active = false;

        /// @brief Pop the include stack when an include exits through any path.
        ~IncludeStackGuard() {
            if (active && stack && !stack->empty())
                stack->pop_back();
        }
    } includeGuard;

    if (includeStack_) {
        if (includeStack_->size() >= static_cast<size_t>(maxIncludeDepth_)) {
            emitError("B0001", kw.loc, "ADDFILE depth limit exceeded");
            return result;
        }
        for (const auto &p : *includeStack_) {
            if (p == canonStr) {
                emitError("B0001", kw.loc, "cyclic ADDFILE detected: " + canonStr);
                return result;
            }
        }
        includeStack_->push_back(canonStr);
        includeGuard.stack = includeStack_;
        includeGuard.active = true;
    }

    // Read file contents.
    std::ifstream in(nativeCanonical);
    if (!in) {
        emitError("B0001", kw.loc, "unable to open: " + canonStr);
        return result;
    }
    constexpr std::uintmax_t kMaxAddFileBytes = 64U * 1024U * 1024U;
    ec.clear();
    const auto fileSize = std::filesystem::file_size(nativeCanonical, ec);
    if (!ec && fileSize > kMaxAddFileBytes) {
        emitError("B0001", kw.loc, "ADDFILE source exceeds 64 MiB limit: " + canonStr);
        return result;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string contents = ss.str();

    uint32_t newFileId = sm_->addFile(canonStr);
    if (newFileId == 0) {
        emitError("B0005", kw.loc, std::string{il::support::kSourceManagerFileIdOverflowMessage});
        return result;
    }
    emitter_->addSource(newFileId, contents);

    // Create and configure child parser.
    Parser child(contents, newFileId, emitter_, sm_, includeStack_, /*suppress*/ true);
    child.arrays_ = arrays_;
    child.knownNamespaces_ = knownNamespaces_;
    child.knownConstInts_ = knownConstInts_;
    child.knownConstStrs_ = knownConstStrs_;
    child.namedLabels_ = namedLabels_;
    child.usedLabelNumbers_ = usedLabelNumbers_;
    child.nextSyntheticLabel_ = nextSyntheticLabel_;

    // Parse the included file.
    auto subprog = child.parseProgram();
    if (!subprog)
        return result;

    // Extract child parser state for caller.
    result.success = true;
    result.subprog = std::move(subprog);
    result.arrays = child.arrays_;
    result.namespaces = child.knownNamespaces_;
    result.constInts = child.knownConstInts_;
    result.constStrs = child.knownConstStrs_;
    result.namedLabels = child.namedLabels_;
    result.usedLabelNumbers = child.usedLabelNumbers_;
    result.nextSyntheticLabel = child.nextSyntheticLabel_;

    return result;
}

/// @brief Handle an `ADDFILE` directive in top-level program context.
/// @details After a successful child parse, moves its procedures and main
///          statements into @p prog and merges the child's parser state back
///          into this parser. Unsupported or failed includes are diagnosed and
///          still count as handled.
/// @param prog Program receiving declarations and statements from the included file.
/// @return `false` when the current token is not `ADDFILE`; otherwise `true`
///         after consuming and either processing or diagnosing the directive.
bool Parser::handleTopLevelAddFile(Program &prog) {
    if (!at(TokenKind::KeywordAddfile))
        return false;

    Token kw = consume(); // ADDFILE

    if (!sm_ || !emitter_) {
        emitError("B0001", kw.loc, "ADDFILE is not supported in this parsing context");
        syncToStmtBoundary();
        if (at(TokenKind::EndOfLine))
            consume();
        return true;
    }

    auto result = processAddFileInclude(kw);
    if (!result.success)
        return true;

    // Merge results into program.
    for (auto &p : result.subprog->procs)
        prog.procs.push_back(std::move(p));
    for (auto &s : result.subprog->main)
        prog.main.push_back(std::move(s));

    // Merge child state back to parent.
    for (const auto &arrName : result.arrays)
        arrays_.insert(arrName);
    for (const auto &nsName : result.namespaces)
        knownNamespaces_.insert(nsName);
    for (const auto &kv : result.constInts)
        knownConstInts_.insert(kv);
    for (const auto &kv : result.constStrs)
        knownConstStrs_.insert(kv);
    namedLabels_ = std::move(result.namedLabels);
    usedLabelNumbers_ = std::move(result.usedLabelNumbers);
    nextSyntheticLabel_ = result.nextSyntheticLabel;

    return true;
}

/// @brief Handle an `ADDFILE` directive inside a statement destination.
/// @details Moves both child procedures and child main statements into @p dst,
///          then merges array, namespace, constant, and label state. Unsupported
///          or failed includes are diagnosed and still count as handled.
/// @param dst Owning statement vector receiving all nodes from the included program.
/// @return `false` when the current token is not `ADDFILE`; otherwise `true`
///         after consuming and either processing or diagnosing the directive.
bool Parser::handleAddFileInto(std::vector<StmtPtr> &dst) {
    if (!at(TokenKind::KeywordAddfile))
        return false;

    Token kw = consume(); // ADDFILE

    if (!sm_ || !emitter_) {
        emitError("B0001", kw.loc, "ADDFILE is not supported in this parsing context");
        syncToStmtBoundary();
        if (at(TokenKind::EndOfLine))
            consume();
        return true;
    }

    auto result = processAddFileInclude(kw);
    if (!result.success)
        return true;

    // Merge results into destination.
    for (auto &p : result.subprog->procs)
        dst.push_back(std::move(p));
    for (auto &s : result.subprog->main)
        dst.push_back(std::move(s));

    // Merge CONSTs back to parent.
    for (const auto &arrName : result.arrays)
        arrays_.insert(arrName);
    for (const auto &nsName : result.namespaces)
        knownNamespaces_.insert(nsName);
    for (const auto &kv : result.constInts)
        knownConstInts_.insert(kv);
    for (const auto &kv : result.constStrs)
        knownConstStrs_.insert(kv);
    namedLabels_ = std::move(result.namedLabels);
    usedLabelNumbers_ = std::move(result.usedLabelNumbers);
    nextSyntheticLabel_ = result.nextSyntheticLabel;

    return true;
}

// ============================================================================
// Error Reporting Helpers
// ============================================================================

/// @brief Emit one parser diagnostic through the configured sink or stderr.
/// @details A configured emitter receives the supplied severity, code, range,
///          and message. Without an emitter, the method prints a simplified
///          `Warning` or `Error` prefix and message to standard error.
/// @param sev Diagnostic severity.
/// @param code Stable diagnostic identifier.
/// @param loc Start of the source range.
/// @param len Length of the highlighted source range in bytes.
/// @param message Human-readable message transferred to the diagnostic sink.
void Parser::emitDiagnostic(il::support::Severity sev,
                            std::string_view code,
                            il::support::SourceLoc loc,
                            uint32_t len,
                            std::string message) {
    if (emitter_) {
        emitter_->emit(sev, std::string(code), loc, len, std::move(message));
    } else {
        const char *prefix = (sev == il::support::Severity::Warning) ? "Warning" : "Error";
        std::fprintf(stderr, "%s: %s\n", prefix, message.c_str());
    }
}

/// @brief Emit an error covering the supplied token lexeme.
/// @param code Stable diagnostic identifier.
/// @param tok Token providing the location and byte length.
/// @param message Human-readable message forwarded to @ref emitDiagnostic.
void Parser::emitError(std::string_view code, const Token &tok, std::string message) {
    emitDiagnostic(il::support::Severity::Error,
                   code,
                   tok.loc,
                   static_cast<uint32_t>(tok.lexeme.size()),
                   std::move(message));
}

/// @brief Emit a zero-length error range at an explicit source location.
/// @param code Stable diagnostic identifier.
/// @param loc Location at which to anchor the diagnostic.
/// @param message Human-readable message forwarded to @ref emitDiagnostic.
void Parser::emitError(std::string_view code, il::support::SourceLoc loc, std::string message) {
    emitDiagnostic(il::support::Severity::Error, code, loc, 0, std::move(message));
}

/// @brief Emit a warning covering the supplied token lexeme.
/// @param code Stable diagnostic identifier.
/// @param tok Token providing the location and byte length.
/// @param message Human-readable message forwarded to @ref emitDiagnostic.
void Parser::emitWarning(std::string_view code, const Token &tok, std::string message) {
    emitDiagnostic(il::support::Severity::Warning,
                   code,
                   tok.loc,
                   static_cast<uint32_t>(tok.lexeme.size()),
                   std::move(message));
}

} // namespace il::frontends::basic
