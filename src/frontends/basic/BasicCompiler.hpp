//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares the BasicCompiler driver class, which orchestrates the
// complete BASIC-to-IL compilation pipeline.
//
// The BasicCompiler provides the top-level entry point for compiling BASIC
// source code into Zanna Intermediate Language (IL) modules, coordinating all
// compilation stages:
//   Lexer → Parser → AST → Semantic → Lowerer → IL
//
// Key Responsibilities:
// - Pipeline orchestration: Coordinates the lexer, parser, semantic analyzer,
//   and lowerer to produce a valid IL module from BASIC source code
// - Diagnostic management: Collects and reports errors/warnings from all
//   compilation stages via a unified DiagnosticEngine
// - Source management: Integrates with the SourceManager for file tracking
//   and location-based error reporting
// - Configuration: Applies compilation options (bounds checking, optimization
//   settings) to control lowering behavior
// - Result packaging: Returns a structured result containing the IL module,
//   diagnostics, and compilation metadata
//
// Compilation Flow:
// 1. Lexical Analysis: Tokenize BASIC source text
// 2. Syntax Analysis: Parse tokens into AST
// 3. Semantic Analysis: Validate AST, resolve symbols, check types
// 4. IL Generation: Lower AST to IL instructions
// 5. IL Verification: Verify the lowered module and forward diagnostics
// 6. Module Finalization: Return IL module with diagnostics
//
// Compilation Options:
// - Bounds checking: Enable/disable runtime array bounds checks (default: off)
// - Diagnostic dumps: Emit tokens, AST, lowered IL, or phase timings
// - Unsafe pointers: Control semantic acceptance of compatibility constructs
//
// Input Specification:
// - Source code: BASIC program text as string_view
// - Path: Source file path for diagnostic messages (optional)
// - File ID: Existing file identifier in SourceManager (optional)
//
// Output Structure:
// The compiler returns a BasicCompilerResult containing:
// - IL module: The lowered program, or a default empty module on early failure
// - Diagnostics: All errors, warnings, and notes from compilation
// - Emitter: Configured DiagnosticEmitter for formatting messages
// - File ID: Source file identifier for cross-referencing
//
// Error Handling:
// - Compilation may produce diagnostics at any stage
// - Early failures leave the value-owned IL module in its default empty state
// - Diagnostics are accumulated and available even on failure
// - The result always includes the DiagnosticEngine for error reporting
//
// Design Notes:
// - The compiler owns the DiagnosticEngine and DiagnosticEmitter
// - The result emitter retains a borrowed SourceManager reference
// - The returned IL module transfers ownership to the caller
// - Multiple compilations can share a single SourceManager
//
// Usage:
//   support::SourceManager srcMgr;
//   BasicCompilerInput input{
//     .source = sourceCode,
//     .path = "program.bas"
//   };
//   BasicCompilerOptions opts{
//     .boundsChecks = true
//   };
//   auto result = compileBasic(input, opts, srcMgr);
//   if (result.succeeded() && result.moduleVerified) {
//     // Compilation succeeded; consume result.module
//   } else {
//     // Inspect result.diagnostics or format through result.emitter
//   }
//
//===----------------------------------------------------------------------===//

/**
 * @file BasicCompiler.hpp
 * @brief Declares the complete BASIC source-to-verified-IL driver contract.
 *
 * Inputs borrow source text and a SourceManager. Results own diagnostics and
 * the module, while their DiagnosticEmitter continues to borrow that manager.
 */

#pragma once

#include "frontends/basic/DiagnosticEmitter.hpp"
#include "support/diagnostics.hpp"
#include "support/source_manager.hpp"
#include "zanna/il/Module.hpp"
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace il::frontends::basic {

/// @brief Options controlling BASIC compilation behavior.
/// @details Some post-optimization dump flags are carried for outer tool
///          drivers; compileBasic() itself consumes bounds, token, AST,
///          pre-optimization IL, timing, and unsafe-pointer settings.
struct BasicCompilerOptions {
    /// @brief Enable debug bounds checks when lowering arrays.
    bool boundsChecks{false};

    /// @brief Dump the raw token stream from the lexer.
    bool dumpTokens{false};

    /// @brief Dump AST after parsing.
    bool dumpAst{false};

    /// @brief Dump IL after lowering, before optimization.
    bool dumpIL{false};

    /// @brief Request an outer driver to dump IL after its optimization pipeline.
    bool dumpILOpt{false};

    /// @brief Request an outer driver to dump IL around each optimization pass.
    bool dumpILPasses{false};

    /// @brief Print frontend phase timings to stderr.
    bool timeCompile{false};

    /// @brief Allow semantic acceptance of unsafe pointer compatibility constructs.
    bool allowUnsafePointers{false};
};

/// @brief Input parameters describing the source to compile.
/// @invariant When @ref fileId is set, @ref path may be empty.
struct BasicCompilerInput {
    /// @brief BASIC source code to compile.
    std::string_view source;
    /// @brief Path used for diagnostics; defaults to "<input>" when empty.
    std::string_view path{"<input>"};
    /// @brief Existing file id within the supplied source manager, if any.
    std::optional<uint32_t> fileId{};
};

/// @brief Aggregated result of compiling BASIC source.
/// @ownership Owns diagnostics, emitter, and module. The emitter borrows the
///            SourceManager passed to compileBasic().
struct BasicCompilerResult {
    /// @brief Diagnostics accumulated during compilation.
    il::support::DiagnosticEngine diagnostics{};
    /// @brief Formatter for diagnostics bound to the provided source manager.
    std::unique_ptr<DiagnosticEmitter> emitter{};
    /// @brief File identifier used for the compiled source.
    uint32_t fileId{0};
    /// @brief Lowered IL module.
    il::core::Module module{};

    /// @brief True when the module has been verified since the last mutation.
    bool moduleVerified{false};

    /// @brief Helper indicating whether compilation succeeded without errors.
    /// @details This predicate checks only that an emitter exists and has
    ///          recorded no errors; it does not require @ref moduleVerified.
    /// @return True when the emitter exists and its error count is zero.
    [[nodiscard]] bool succeeded() const;
};

/// @brief Compile BASIC source text into IL.
/// @details Registers or reuses the input file, optionally dumps tokens, parses,
///          collects qualified procedures, folds constants, runs semantic
///          analysis, lowers to IL, and verifies up to 50 IL diagnostics.
///          Fatal or error-producing stages return early with accumulated
///          diagnostics. Runtime-namespace enablement is reset from
///          `ZANNA_NO_RUNTIME_NAMESPACES` for each invocation.
/// @param input Source information describing the buffer to compile.
/// @param options Front-end options controlling lowering behavior.
/// @param sm Source manager used for diagnostics and tracing.
/// @return Module and diagnostics emitted during compilation.
/// @warning @p sm must outlive the returned result because the result's emitter
///          retains a reference to it.
/// @note The function mutates @p sm and global FrontendOptions state.
BasicCompilerResult compileBasic(const BasicCompilerInput &input,
                                 const BasicCompilerOptions &options,
                                 il::support::SourceManager &sm);

} // namespace il::frontends::basic
