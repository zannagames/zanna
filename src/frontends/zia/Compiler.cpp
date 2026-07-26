//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/zia/Compiler.cpp
// Purpose: Orchestrate the Zia lexing, parsing, bind resolution, semantic
//          analysis, lowering, verification, and optimization pipeline.
// Key invariants:
//   * A phase does not run after a fatal failure in an earlier phase.
//   * CompilerResult::moduleVerified reflects verification since the module's
//     most recent mutation.
//   * Error-tolerant analysis keeps its DiagnosticEngine at a stable address
//     while Sema borrows it.
// Ownership: CompilerResult owns diagnostics and IL output; parseAndAnalyze()
//            returns a heap-owned aggregate whose Sema borrows its diagnostics.
// References: docs/languages/zia-reference.md, docs/tools/cli.md
//
//===----------------------------------------------------------------------===//
///
/// @file
/// @brief Implementation of Zia compiler driver.
///
/// @details This file implements the compile() and compileFile() functions
/// that orchestrate the complete compilation pipeline. Key implementation:
///
/// ## Import Resolution
///
/// ImportResolver recursively resolves file binds:
/// 1. Resolves import paths relative to the importing file
/// 2. Parses each imported file
/// 3. Recursively processes that file's imports
/// 4. Prepends imported declarations to the importing module
///
/// Import path resolution:
/// - "./foo" or "../bar" → Relative to importing file
/// - "foo" → Same directory as importing file, add .zia extension
///
/// ## Safety Guards
///
/// To prevent runaway compilation:
/// - Maximum import depth: 50 levels
/// - Maximum imported files: 256
/// - Circular import detection via processedFiles set
///
/// ## Compilation Phases
///
/// The compile() function executes phases in order:
/// 1. Create Lexer from source
/// 2. Parse with Parser to get AST
/// 3. Process imports (load and merge)
/// 4. Semantic analysis with Sema
/// 5. Lower to IL with Lowerer
///
/// @see Compiler.hpp for the public API
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Compiler.hpp"
#include "common/Filesystem.hpp"
#include "frontends/zia/ImportResolver.hpp"
#include "frontends/zia/Lexer.hpp"
#include "frontends/zia/Lowerer.hpp"
#include "frontends/zia/Parser.hpp"
#include "frontends/zia/Sema.hpp"
#include "frontends/zia/ZiaAnalysis.hpp"
#include "frontends/zia/ZiaAstPrinter.hpp"
#include "il/transform/PassManager.hpp"
#include "il/verify/Verifier.hpp"
#include "zanna/il/IO.hpp"
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>

namespace il::frontends::zia {

namespace {
/// @brief Report all verifier diagnostics and return whether verification had no errors.
/// @param module IL module to verify without modification.
/// @param diagnostics Engine that receives every verifier diagnostic.
/// @return True when no error-severity verifier diagnostic was produced.
bool reportVerifierDiagnostics(const il::core::Module &module,
                               il::support::DiagnosticEngine &diagnostics) {
    bool hasError = false;
    for (auto diag : il::verify::Verifier::verifyAll(module, 50)) {
        if (diag.severity == il::support::Severity::Error)
            hasError = true;
        diagnostics.report(std::move(diag));
    }
    return !hasError;
}

/// @brief Print every token from the source to stderr.
/// @details Creates a fresh lexer and iterates until EOF, printing each token
///          with its location, kind, text, and literal values.
/// @param source Source buffer to lex independently of the compiler parser.
/// @param fileId SourceManager identifier associated with @p source.
/// @param diag Diagnostic sink used by the temporary lexer.
void dumpTokenStream(const std::string &source,
                     uint32_t fileId,
                     il::support::DiagnosticEngine &diag) {
    Lexer lexer(source, fileId, diag);
    std::cerr << "=== Zia Token Stream ===\n";
    for (;;) {
        Token tok = lexer.next();
        std::cerr << tok.loc.line << ':' << tok.loc.column << '\t' << tokenKindToString(tok.kind);
        if (!tok.text.empty())
            std::cerr << "\t\"" << tok.text << '"';
        if (tok.kind == TokenKind::IntegerLiteral)
            std::cerr << "\tvalue=" << tok.intValue;
        else if (tok.kind == TokenKind::NumberLiteral)
            std::cerr << "\tvalue=" << tok.floatValue;
        std::cerr << '\n';
        if (tok.kind == TokenKind::Eof)
            break;
    }
    std::cerr << "=== End Token Stream ===\n";
}
} // namespace

/// @brief Check whether compilation completed without error diagnostics.
/// @return True when diagnostics.errorCount() is zero.
bool CompilerResult::succeeded() const {
    return diagnostics.errorCount() == 0;
}

/// @brief Compile one Zia source buffer through the configured pipeline.
/// @details Registers the source, parses its AST, resolves file binds, performs
///          semantic analysis and lowering, then optionally verifies, optimizes,
///          times, or dumps intermediate representations according to @p options.
/// @param input Source buffer, path, optional existing file ID, and import provider.
/// @param options Frontend, diagnostic, verification, and optimization controls.
/// @param sm Source manager that owns file identities and source text.
/// @return Compilation artifacts and all diagnostics accumulated before return.
CompilerResult compile(const CompilerInput &input,
                       const CompilerOptions &options,
                       il::support::SourceManager &sm) {
    CompilerResult result{};
    auto phaseStart = std::chrono::steady_clock::now();
    /// @brief Prints elapsed time for one compiler phase when timing is enabled.
    /// @param phase Phase name appended to the timing prefix.
    auto printPhaseTime = [&](const char *phase) {
        if (!options.timeCompile)
            return;
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - phaseStart);
        std::cerr << "[time-compile] zia." << phase << " " << elapsed.count() << "ms\n";
        phaseStart = std::chrono::steady_clock::now();
    };

    // Register source file if not already registered
    if (input.fileId.has_value()) {
        result.fileId = *input.fileId;
    } else {
        result.fileId = sm.addFile(std::string(input.path));
    }
    if (result.fileId == 0) {
        result.diagnostics.report({il::support::Severity::Error,
                                   std::string{il::support::kSourceManagerFileIdOverflowMessage},
                                   {},
                                   "V-SRC-FILE-ID"});
        return result;
    }
    sm.setSource(result.fileId, std::string(input.source));
    printPhaseTime("source-manager");

    // Debug timing
    /// @brief Prints a phase marker when debug compilation tracing is enabled.
    /// @param phase Phase name to print.
    auto debugTime = [](const char *phase) {
        if (std::getenv("ZIA_DEBUG_COMPILE") != nullptr)
            std::cerr << "[zia] " << phase << std::endl;
    };

    // Phase 0 (optional): Token stream dump — uses a separate lexer so parsing
    // still works from the original one.
    if (options.dumpTokens) {
        dumpTokenStream(std::string(input.source), result.fileId, result.diagnostics);
        if (result.diagnostics.errorCount() > 0)
            return result;
    }
    printPhaseTime("tokens");

    debugTime("Phase 1: Lexing");
    // Phase 1: Lexing
    Lexer lexer(std::string(input.source), result.fileId, result.diagnostics);

    debugTime("Phase 2: Parsing");
    // Phase 2: Parsing
    Parser parser(lexer, result.diagnostics);
    auto module = parser.parseModule();

    if (!module || parser.hasError() || result.diagnostics.errorCount() > 0) {
        // Parse failed, return with diagnostics
        return result;
    }
    printPhaseTime("parse");

    // Dump AST after parsing (before sema).
    if (options.dumpAst) {
        ZiaAstPrinter printer;
        std::cerr << "=== AST after parsing ===\n" << printer.dump(*module) << "=== End AST ===\n";
    }

    Sema sema(result.diagnostics);
    sema.setAllowUnsafePointers(options.allowUnsafePointers);
    sema.initWarnings(options.warningPolicy);
    sema.addWarningSuppressions(result.fileId, input.source);

    debugTime("Phase 2.5: Import resolution");
    // Phase 2.5: Process binds (load and merge bound files)
    if (!module->binds.empty()) {
        ImportResolver resolver(
            result.diagnostics, sm, &sema.warningSuppressions(), input.sourceProvider);
        if (!resolver.resolve(*module, std::string(input.path))) {
            // Import processing failed
            return result;
        }
        if (result.diagnostics.errorCount() > 0)
            return result;
    }
    printPhaseTime("imports");

    debugTime("Phase 3: Semantic Analysis");
    // Phase 3: Semantic Analysis
    bool semanticOk = sema.analyze(*module);
    printPhaseTime("sema");

    // Dump AST after semantic analysis.
    if (options.dumpSemaAst) {
        ZiaAstPrinter printer;
        std::cerr << "=== AST after semantic analysis ===\n"
                  << printer.dump(*module) << "=== End AST ===\n";
    }

    if (!semanticOk || result.diagnostics.errorCount() > 0) {
        // Semantic analysis failed, return with diagnostics
        return result;
    }

    debugTime("Phase 4: IL Lowering");
    // Phase 4: IL Lowering
    Lowerer lowerer(sema, result.diagnostics, options);
    result.module = lowerer.lower(*module);
    if (result.diagnostics.errorCount() > 0)
        return result;
    if (options.captureDebugLayouts)
        result.debugClassLayouts = lowerer.collectDebugClassLayouts();
    result.moduleVerified = false;
    printPhaseTime("lower");
    if (options.verifyAfterLowering) {
        if (!reportVerifierDiagnostics(result.module, result.diagnostics)) {
            return result;
        }
        result.moduleVerified = true;
    }
    printPhaseTime("verify-lower");
    debugTime("Phase 4: Done");

    // Dump IL after lowering, before optimization.
    if (options.dumpIL) {
        std::cerr << "=== IL after lowering ===\n";
        io::Serializer::write(result.module, std::cerr);
        std::cerr << "=== End IL ===\n";
    }

    // Phase 5: IL Optimization — use the canonical registered pipelines.
    // O1 and O2 pipelines are defined in PassManager's constructor and include
    // the full sequence of passes (SCCP, LICM, loop transforms, inlining, etc.).
    if (options.optLevel != OptLevel::O0) {
        il::transform::PassManager pm;
        pm.setVerifyBetweenPasses(options.verifyEachPass);
        pm.setReportPassStatistics(options.passStats);
        pm.setInstrumentationStream(std::cerr);
        pm.enableParallelFunctionPasses(options.parallelFunctionPasses && !options.verifyEachPass &&
                                        !options.dumpILPasses);

        // Enable per-pass IL dumps when requested.
        if (options.dumpILPasses) {
            pm.setPrintBeforeEach(true);
            pm.setPrintAfterEach(true);
        }

        const std::string pipelineId = (options.optLevel == OptLevel::O2) ? "O2" : "O1";
        result.moduleVerified = false;
        if (!pm.runPipeline(result.module, pipelineId)) {
            result.diagnostics.report(
                {il::support::Severity::Error,
                 "IL optimization pipeline '" + pipelineId + "' failed verification",
                 {},
                 "V-OPT-PIPELINE"});
            return result;
        }
        printPhaseTime("optimize");

        if (options.verifyAfterOptimization) {
            if (!reportVerifierDiagnostics(result.module, result.diagnostics)) {
                return result;
            }
            result.moduleVerified = true;
        }
        printPhaseTime("verify-opt");
    }

    // Dump IL after the full optimization pipeline.
    if (options.dumpILOpt) {
        const char *level = (options.optLevel == OptLevel::O2)   ? "O2"
                            : (options.optLevel == OptLevel::O1) ? "O1"
                                                                 : "O0";
        std::cerr << "=== IL after optimization (" << level << ") ===\n";
        io::Serializer::write(result.module, std::cerr);
        std::cerr << "=== End IL ===\n";
    }

    return result;
}

/// @brief Read and compile a Zia source file.
/// @details Loads the complete file in binary mode, reports I/O failures as
///          compiler diagnostics, and delegates the source buffer to compile().
/// @param path UTF-8 path to the root `.zia` file.
/// @param options Frontend and pipeline controls.
/// @param sm Source manager that receives the loaded root file.
/// @return Compilation result, including I/O or pipeline diagnostics.
CompilerResult compileFile(const std::string &path,
                           const CompilerOptions &options,
                           il::support::SourceManager &sm) {
    auto readStart = std::chrono::steady_clock::now();
    // Read file contents
    std::ifstream file(zanna::filesystem::pathFromUtf8(path), std::ios::binary | std::ios::ate);
    if (!file) {
        CompilerResult result{};
        result.diagnostics.report({il::support::Severity::Error,
                                   "Failed to open file: " + path,
                                   il::support::SourceLoc{},
                                   "V1000"});
        return result;
    }

    const auto size = file.tellg();
    file.seekg(0);
    if (size < 0) {
        CompilerResult result{};
        result.diagnostics.report({il::support::Severity::Error,
                                   "Failed to determine file size: " + path,
                                   il::support::SourceLoc{},
                                   "V1000"});
        return result;
    }
    std::string source(static_cast<std::size_t>(size), '\0');
    if (size > 0)
        file.read(source.data(), size);
    if (!file) {
        CompilerResult result{};
        result.diagnostics.report({il::support::Severity::Error,
                                   "Failed to read file: " + path,
                                   il::support::SourceLoc{},
                                   "V1000"});
        return result;
    }
    if (options.timeCompile) {
        const auto elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readStart);
        std::cerr << "[time-compile] zia.read " << elapsed.count() << "ms\n";
    }

    CompilerInput input;
    input.source = source;
    input.path = path;

    return compile(input, options, sm);
}

/// @brief Parse and semantically analyze a source buffer for tooling.
/// @details Preserves partial AST and Sema state after recoverable errors, runs
///          bind resolution best-effort, and deliberately omits IL lowering.
/// @param input Source buffer and import-provider information.
/// @param options Warning and semantic-analysis controls.
/// @param sm Source manager that owns file identities and source text.
/// @return Stable heap-owned partial analysis result; the pointer is non-null
///         even when diagnostics contain errors.
std::unique_ptr<AnalysisResult> parseAndAnalyze(const CompilerInput &input,
                                                const CompilerOptions &options,
                                                il::support::SourceManager &sm) {
    // Heap-allocate the result so DiagnosticEngine has a stable address.
    // Sema holds a reference to it; moving a unique_ptr never relocates the
    // pointed-to object, so the reference remains valid for the object's lifetime.
    auto result = std::make_unique<AnalysisResult>();

    // Register source file (matches the logic in compile()).
    uint32_t fileId =
        input.fileId.has_value() ? *input.fileId : sm.addFile(std::string(input.path));
    if (fileId == 0) {
        result->diagnostics.report({il::support::Severity::Error,
                                    std::string{il::support::kSourceManagerFileIdOverflowMessage},
                                    {},
                                    "V-SRC-FILE-ID"});
        return result;
    }
    sm.setSource(fileId, std::string(input.source));

    // Phase 1: Lexing
    Lexer lexer(std::string(input.source), fileId, result->diagnostics);

    // Phase 2: Parsing — continue on errors for tolerance.
    // Parser::parseModule() accumulates errors in result->diagnostics and
    // attempts to return a partial AST via resync-after-error recovery.
    Parser parser(lexer, result->diagnostics);
    auto module = parser.parseModule();

    if (!module) {
        // Complete parse failure — no AST to analyze.
        return result;
    }

    result->ast = std::move(module);

    result->fileId = fileId;

    result->sema = std::make_unique<Sema>(result->diagnostics);
    result->sema->initWarnings(options.warningPolicy);
    result->sema->addWarningSuppressions(fileId, input.source);

    // Phase 2.5: Import resolution (best-effort).
    // Failures are accumulated in diagnostics but do not abort analysis.
    if (!result->ast->binds.empty()) {
        ImportResolver resolver(
            result->diagnostics, sm, &result->sema->warningSuppressions(), input.sourceProvider);
        resolver.resolve(*result->ast, std::string(input.path));
    }

    // Phase 3: Semantic analysis.
    // We always construct and run Sema — even when there were parse errors —
    // because partial type resolution is still valuable for completions.
    // DiagnosticEngine address is stable (heap-allocated in result).
    result->sema->analyze(*result->ast);
    // Ignore false return: partial Sema state is the desired output.

    return result;
}

} // namespace il::frontends::zia
