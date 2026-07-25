//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/BasicAnalysis.hpp
// Purpose: Partial-compilation API for BASIC IDE tooling (completion, hover, etc.).
// Key invariants:
//   - Runs parse → CollectProcedures → foldConstants → SemanticAnalyzer
//   - Stops before lowering (no IL generation)
//   - Result is heap-allocated for stable DiagnosticEngine address
// Ownership/Lifetime:
//   - AnalysisResult owns diagnostics, emitter, AST, and sema
//   - Destruction order: sema → ast → emitter → diagnostics (reverse declaration)
//   - The SourceManager passed to parseAndAnalyzeBasic must outlive the result
// Links: src/frontends/basic/BasicCompiler.hpp,
//        src/frontends/basic/SemanticAnalyzer.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file BasicAnalysis.hpp
 * @brief Declares the parse-and-semantics BASIC pipeline used by IDE tooling.
 *
 * The partial pipeline retains diagnostics, the parsed program, and a live
 * semantic analyzer but deliberately performs no lowering or IL generation.
 */

#pragma once

#include "frontends/basic/BasicCompiler.hpp"
#include "frontends/basic/SemanticAnalyzer.hpp"

#include <memory>

namespace il::frontends::basic {

/// Parsed BASIC program root.
struct Program;

/// @brief Result of a partial BASIC compilation run (parse + sema only).
///
/// Provides access to the resolved symbol tables even when the source has
/// errors. Callers should query `sema` for completion/hover information and
/// inspect `diagnostics` for error details.
///
/// Heap-allocated to ensure stable DiagnosticEngine address (SemanticAnalyzer
/// holds a reference to the emitter which references diagnostics). The emitter
/// also borrows the SourceManager supplied to parseAndAnalyzeBasic().
struct BasicAnalysisResult {
    /// @brief Diagnostics accumulated during parsing and semantic analysis.
    /// @note Declared first so it is destroyed last.
    il::support::DiagnosticEngine diagnostics{};

    /// @brief Formatter for diagnostics.
    std::unique_ptr<DiagnosticEmitter> emitter;

    /// @brief The parsed AST (owned).
    /// @details May be nullptr if the parser cannot produce any output.
    std::unique_ptr<Program> ast;

    /// @brief The semantic analyzer after analysis (owned).
    /// @details Non-null whenever `ast` is non-null.
    std::unique_ptr<SemanticAnalyzer> sema;

    /// @brief File identifier for the analyzed source.
    uint32_t fileId{0};

    /// @brief True if any errors were reported.
    /// @return Whether the owned diagnostic engine's error count is nonzero.
    [[nodiscard]] bool hasErrors() const {
        return diagnostics.errorCount() > 0;
    }
};

/// @brief Run the BASIC pipeline through semantic analysis only.
///
/// Executes Lexer → Parser → CollectProcedures → foldConstants → SemanticAnalyzer,
/// stopping before lowering. Returns a heap-allocated result containing the
/// analyzed AST and a live SemanticAnalyzer whose symbol tables can be queried
/// for IDE features.
///
/// @param input  Source information (code text + path + optional file id).
/// @param sm     Source manager for file registration and diagnostics.
/// @return       Heap-allocated result with AST, sema, and diagnostics.
/// @warning @p sm must outlive the returned result because its DiagnosticEmitter
///          retains a reference to the source manager.
/// @post When parsing produces a program, both `result->ast` and
///       `result->sema` are non-null even if diagnostics contain errors.
std::unique_ptr<BasicAnalysisResult> parseAndAnalyzeBasic(const BasicCompilerInput &input,
                                                          il::support::SourceManager &sm);

} // namespace il::frontends::basic
