//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file ZiaAnalysis.hpp
/// @brief Partial-compilation API for Zia IDE tooling (completion, hover, etc.).
///
/// @details This header exposes `AnalysisResult` and `parseAndAnalyze()`, which
/// run the Zia pipeline through semantic analysis only — stopping before IL
/// lowering and optimization. The result is an owned `Sema` object whose
/// symbol tables can be queried to implement code completion, hover information,
/// go-to-definition, and other editor features.
///
/// ## Design Notes
///
/// This header is intentionally kept separate from `Compiler.hpp` to avoid
/// pulling in `Sema.hpp` (and transitively `AST.hpp`) into the 69+ files that
/// include `Compiler.hpp`. Only files that implement IDE tooling need to include
/// `ZiaAnalysis.hpp`.
///
/// ## Ownership and Lifetime
///
/// `AnalysisResult` member declaration order is significant for correct
/// destruction:
///   1. `diagnostics` — destroyed LAST  (Sema holds a reference to it)
///   2. `ast`         — destroyed second (Sema holds raw pointers into the AST)
///   3. `sema`        — destroyed FIRST  (holds refs to diagnostics and ast)
///
/// C++ destroys members in reverse declaration order, so declaring `sema`
/// last achieves destruction first.
///
/// ## Error Tolerance
///
/// `parseAndAnalyze()` continues even when the source contains errors:
/// - **Parse errors**: Sema still analyzes the partial AST.
/// - **Sema errors**: The Sema object retains all successfully-resolved types.
/// - **Null AST**: Only when the parser cannot produce any output (returned
///   early); callers should check `!result.ast` before querying `result.sema`.
///
/// ## Usage
///
/// ```cpp
/// #include "frontends/zia/ZiaAnalysis.hpp"
///
/// SourceManager sm;
/// CompilerInput input{.source = editorText, .path = "main.zia"};
/// CompilerOptions opts{};
///
/// auto ar = parseAndAnalyze(input, opts, sm);
/// auto members = ar.sema->getMembersOf(someType);
/// ```
///
/// @see Compiler.hpp  — `compile()` / `compileFile()` for full compilation
/// @see Sema.hpp      — Completion query APIs (`getMembersOf`, etc.)
///
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/zia/Compiler.hpp" // CompilerInput, CompilerOptions
#include "frontends/zia/Sema.hpp"     // Sema (+ Symbol, TypeRef via transitive includes)
#include <memory>

namespace il::frontends::zia {

// ModuleDecl is defined in AST_Decl.hpp, which Sema.hpp includes transitively
// via AST.hpp. No extra include needed.

/// @brief Result of a partial Zia compilation run (parse + sema only).
/// @details Returned by `parseAndAnalyze()` as a heap-allocated object
/// (via `unique_ptr<AnalysisResult>`). Provides access to the resolved symbol
/// tables even when the source has errors. Callers should query `sema` for
/// completion information and inspect `diagnostics` for error details.
///
/// ## Why heap-allocated?
///
/// `Sema` holds a **reference** to `diagnostics` (`DiagnosticEngine &diag_`).
/// If `AnalysisResult` were returned by value and then moved into a cache, the
/// move would change the address of `diagnostics` while `sema->diag_` still
/// points to the old address — a dangling reference. Heap-allocating the whole
/// struct ensures `diagnostics` stays at a stable address for the object's
/// lifetime, and only the cheap pointer is ever moved.
///
/// ## Destruction order
///
/// C++ destroys struct members in reverse declaration order:
///   `sema` → `ast` → `diagnostics`
///
/// This ensures:
/// - `sema` (holds `&diagnostics`) is destroyed before `diagnostics`.
/// - `sema` (holds raw declaration pointers into `ast`) is destroyed before `ast`.
struct AnalysisResult {
    /// @brief Diagnostics accumulated during parsing and semantic analysis.
    /// @note Declared first so it is destroyed last (after sema and ast).
    il::support::DiagnosticEngine diagnostics{};

    /// @brief The parsed and import-resolved AST (owned).
    /// @details May be nullptr if the parser cannot produce any AST output
    /// (catastrophic parse failure). Sema has raw pointers into this tree, so
    /// ast must outlive sema — ensured by declaration order.
    std::unique_ptr<ModuleDecl> ast;

    /// @brief The semantic analyzer after analysis (owned).
    /// @details Non-null whenever `ast` is non-null. Holds a reference to
    /// `diagnostics`, so declared last (destroyed first).
    std::unique_ptr<Sema> sema;

    /// @brief SourceManager file id for the primary source being analyzed.
    uint32_t fileId{0};

    /// @brief True if any errors were reported during parsing or sema.
    [[nodiscard]] bool hasErrors() const {
        return diagnostics.errorCount() > 0;
    }
};

/// @brief Run the Zia pipeline through semantic analysis (stages 1–4).
///
/// @details Executes Lexer → Parser → ImportResolver → Sema, stopping before
/// IL lowering. Returns a heap-allocated `AnalysisResult` (via `unique_ptr`)
/// containing the analyzed AST and a live `Sema` object whose symbol tables
/// can be queried for IDE features.
///
/// The result is heap-allocated to guarantee a stable address for the
/// `DiagnosticEngine` member, which `Sema` holds by reference. Moving a
/// `unique_ptr` only moves the pointer — the pointed-to object never relocates.
///
/// Error tolerance:
/// - Parse errors are accumulated in `result->diagnostics`; analysis continues
///   on the partial AST whenever possible.
/// - Sema errors are likewise accumulated; the Sema object retains all type
///   information successfully resolved up to the point of each error.
/// - Import resolution failures are non-fatal; missing imported symbols are
///   simply absent from the module's scope.
///
/// @param input  Source information (code text + path + optional file id).
/// @param options Compiler options (bounds/overflow/null check flags, etc.).
/// @param sm     Source manager for file registration and diagnostics.
/// @return       Heap-allocated AnalysisResult with AST, Sema, and diagnostics.
///               Never returns nullptr; always contains a non-null AnalysisResult.
std::unique_ptr<AnalysisResult> parseAndAnalyze(const CompilerInput &input,
                                                const CompilerOptions &options,
                                                il::support::SourceManager &sm);

} // namespace il::frontends::zia
