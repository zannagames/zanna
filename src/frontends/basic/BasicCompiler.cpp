//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/BasicCompiler.cpp
// Purpose: Implements the BASIC front-end driver responsible for parsing,
//          analysing, and lowering source programs into IL modules. The
//          translation unit stitches the individual pipeline stages together
//          while surfacing diagnostics to callers.
// Key invariants: Pipeline stages run in a strict order (parse → fold → sema →
//                 lower) and abort early on fatal diagnostics.
// Ownership/Lifetime: Diagnostic emitters and modules are owned by
//                     BasicCompilerResult; all other helpers are stack scoped.
// Links: src/frontends/basic/BasicCompiler.hpp,
//        src/frontends/basic/Parser.hpp,
//        src/frontends/basic/SemanticAnalyzer.hpp,
//        src/frontends/basic/Lowerer.hpp
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief BASIC front-end compilation pipeline.
/// @details Provides the high-level entry point that runs the parser, constant
///          folder, semantic analyser, and lowerer in sequence.  Results are
///          returned as a @ref BasicCompilerResult containing diagnostics and the
///          generated module.

#include "frontends/basic/BasicCompiler.hpp"

#include "frontends/basic/AstPrinter.hpp"
#include "frontends/basic/ConstFolder.hpp"
#include "frontends/basic/Lexer.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/Options.hpp"
#include "frontends/basic/Parser.hpp"
#include "frontends/basic/SemanticAnalyzer.hpp"
#include "frontends/basic/passes/CollectProcs.hpp"
#include "il/verify/Verifier.hpp"
#include "zanna/il/IO.hpp"
#include <chrono>
#include <iostream>

namespace il::frontends::basic {

namespace {
/// @brief Print every token from the BASIC source to stderr.
/// @details Lexes through the end-of-file token and emits source coordinates,
///          token names, and non-empty lexemes between stable banner lines.
/// @param source BASIC source bytes to lex.
/// @param fileId File identifier embedded in token locations.
void dumpTokenStream(std::string_view source, uint32_t fileId) {
    Lexer lexer(source, fileId);
    std::cerr << "=== BASIC Token Stream ===\n";
    for (;;) {
        Token tok = lexer.next();
        std::cerr << tok.loc.line << ':' << tok.loc.column << '\t' << tokenKindToString(tok.kind);
        if (!tok.lexeme.empty())
            std::cerr << "\t\"" << tok.lexeme << '"';
        std::cerr << '\n';
        if (tok.kind == TokenKind::EndOfFile)
            break;
    }
    std::cerr << "=== End Token Stream ===\n";
}

/// @brief Forward an IL verifier diagnostic through the BASIC diagnostic emitter.
/// @details Uses B9001 when the verifier supplies no code and prefixes the
///          verifier message with BASIC-lowering context.
/// @param result Compilation result whose emitter receives the diagnostic.
/// @param diag Verifier diagnostic to translate.
void emitVerifierDiagnostic(BasicCompilerResult &result, const il::support::Diag &diag) {
    const std::string code = diag.code.empty() ? "B9001" : diag.code;
    result.emitter->emit(
        diag.severity, code, diag.loc, 1, "invalid IL after BASIC lowering: " + diag.message);
}

/// @brief Forward all IL verifier diagnostics through the BASIC diagnostic emitter.
/// @details Verifies the current module with a 50-diagnostic limit and forwards
///          every returned diagnostic, including non-error severities.
/// @param result Compilation result containing the module and destination emitter.
/// @return True when the verifier returned no error-severity diagnostic.
bool reportVerifierDiagnostics(BasicCompilerResult &result) {
    bool hasError = false;
    for (const auto &diag : il::verify::Verifier::verifyAll(result.module, 50)) {
        if (diag.severity == il::support::Severity::Error)
            hasError = true;
        emitVerifierDiagnostic(result, diag);
    }
    return !hasError;
}
} // namespace

/// @brief Report whether the compilation pipeline produced a valid module.
///
/// @details The compiler front end records every diagnostic through the shared
///          emitter stored on the result.  Success therefore requires both an
///          initialised emitter (meaning the pipeline executed far enough to set
///          it up) and an empty error stream.  Downstream stages call this helper
///          before attempting to inspect or emit IL so malformed programs never
///          proceed to lowering.
///
/// @return True when compilation completed without recorded errors; otherwise
///         false.
[[nodiscard]] bool BasicCompilerResult::succeeded() const {
    return emitter && emitter->errorCount() == 0;
}

/// @copydoc compileBasic()
BasicCompilerResult compileBasic(const BasicCompilerInput &input,
                                 const BasicCompilerOptions &options,
                                 il::support::SourceManager &sm) {
    BasicCompilerResult result{};
    auto phaseStart = std::chrono::steady_clock::now();
    /// Optionally report elapsed time since the preceding phase marker.
    auto printPhaseTime = [&](const char *phase) {
        if (!options.timeCompile)
            return;
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - phaseStart);
        std::cerr << "[time-compile] basic." << phase << " " << elapsed.count() << "ms\n";
        phaseStart = std::chrono::steady_clock::now();
    };
    result.emitter = std::make_unique<DiagnosticEmitter>(result.diagnostics, sm);

    uint32_t fileId = input.fileId.value_or(0);
    if (fileId == 0) {
        std::string path = input.path.empty() ? std::string{"<input>"} : std::string{input.path};
        fileId = sm.addFile(std::move(path));
    }
    result.fileId = fileId;

    if (fileId == 0) {
        result.emitter->emit(il::support::Severity::Error,
                             "B0005",
                             {},
                             0,
                             "source manager exhausted file identifier space");
        return result;
    }

    result.emitter->addSource(fileId, std::string{input.source});
    sm.setSource(fileId, std::string{input.source});
    printPhaseTime("source-manager");

    // Dump token stream before parsing if requested.
    if (options.dumpTokens) {
        dumpTokenStream(input.source, fileId);
    }
    printPhaseTime("tokens");

    // Runtime namespaces feature is controlled globally via FrontendOptions (default ON).
    // Allow disabling via environment variable for CLI/debugging.
    const char *ns_disable = std::getenv("ZANNA_NO_RUNTIME_NAMESPACES");
    if (ns_disable && ns_disable[0] == '1')
        FrontendOptions::setEnableRuntimeNamespaces(false);
    else
        FrontendOptions::setEnableRuntimeNamespaces(true);

    // Prepare include stack for ADDFILE handling.
    std::vector<std::string> includeStack;
    Parser parser(
        input.source, fileId, result.emitter.get(), &sm, &includeStack, /*suppress*/ false);
    auto program = parser.parseProgram();
    if (!program) {
        return result;
    }
    printPhaseTime("parse");

    // Dump AST after parsing.
    if (options.dumpAst) {
        AstPrinter printer;
        std::cerr << "=== AST after parsing ===\n" << printer.dump(*program) << "=== End AST ===\n";
    }

    // Post-parse: assign qualified names to procedures inside namespaces.
    // This enables semantic analysis to register nested procedures by their
    // fully-qualified names.
    CollectProcedures(*program);

    foldConstants(*program);
    printPhaseTime("fold");

    SemanticAnalyzer sema(*result.emitter);
    sema.setAllowUnsafePointers(options.allowUnsafePointers);
    sema.analyze(*program);
    printPhaseTime("sema");

    if (!result.succeeded()) {
        return result;
    }

    Lowerer lower(options.boundsChecks);
    lower.setDiagnosticEmitter(result.emitter.get());
    lower.setSemanticAnalyzer(&sema);
    result.module = lower.lower(*program);
    result.moduleVerified = false;
    if (!result.succeeded()) {
        return result;
    }
    printPhaseTime("lower");

    if (!reportVerifierDiagnostics(result)) {
        return result;
    }
    result.moduleVerified = true;
    printPhaseTime("verify-lower");

    // Dump IL after lowering, before optimization.
    if (options.dumpIL) {
        std::cerr << "=== IL after lowering ===\n";
        io::Serializer::write(result.module, std::cerr);
        std::cerr << "=== End IL ===\n";
    }

    return result;
}

} // namespace il::frontends::basic
