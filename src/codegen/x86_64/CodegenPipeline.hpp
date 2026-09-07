//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/CodegenPipeline.hpp
// Purpose: Declare a reusable pipeline that lowers IL modules to native code.
// Key invariants:
//   - Passes execute sequentially with early exit on failure.
//   - Assembly is written before linking when emit_asm is set.
//   - run() returns a zero exit_code only when all stages succeed.
//   - optimize level controls backend optimization passes (0=none, 1+=scheduler+peephole).
// Ownership/Lifetime:
//   - Callers retain ownership of file paths; pipeline state is local to each run().
// Links: src/codegen/x86_64/CodegenPipeline.cpp,
//        src/codegen/x86_64/Backend.hpp,
//        src/codegen/x86_64/passes/PassManager.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/x86_64/passes/PassManager.hpp"

#include <optional>
#include <string>
#include <vector>

/**
 * @file
 * @brief Declares the end-to-end x86-64 compilation and linking pipeline.
 *
 * CodegenPipeline is the CLI-facing orchestration layer above the reusable
 * backend passes. It can load IL itself or consume an in-memory module, select
 * native or system assembly, produce an object or executable, optionally embed
 * an asset blob, and optionally execute the linked program.
 */

/**
 * @brief Captured process-style result of a pipeline invocation.
 *
 * The pipeline does not write subprocess output directly to the caller's
 * streams. Instead it aggregates normal output and diagnostics here so command
 * frontends can route them consistently.
 */
struct PipelineResult {
    int exit_code{0};        ///< Final exit status of the pipeline; zero indicates success.
    std::string stdout_text; ///< Aggregated standard output emitted by subprocesses.
    std::string stderr_text; ///< Aggregated diagnostics and error messages.
};

namespace zanna::codegen::x64 {

/**
 * @brief High-level orchestrator for the x86-64 code-generation flow.
 *
 * An instance owns an immutable-by-convention copy of its options. Each call to
 * @ref run or @ref runWithModule creates fresh module, pass-manager, emission,
 * and linking state.
 */
class CodegenPipeline {
  public:
    /// @brief Selects how assembly or machine code is materialized.
    enum class AssemblerMode {
        System, ///< Use the host assembler/compiler driver.
        Native, ///< Use the native binary encoder + object writer.
    };

    /**
     * @brief Selects the requested link driver.
     * @details System mode is retained for option compatibility but currently
     *          warns and delegates to the built-in linker.
     */
    enum class LinkMode {
        System, ///< Use the host toolchain linker.
        Native, ///< Use the built-in Zanna native linker.
    };

    /**
     * @brief User-configurable input, output, target, and execution policy.
     *
     * Default construction selects native machine-code emission and native
     * linking for the host ABI/platform at optimization level one.
     */
    struct Options {
        std::string input_il_path;         ///< IL module to load and compile.
        std::string output_obj_path;       ///< Destination path for executable/object output.
        std::string output_asm_path;       ///< Optional assembly output path when emit_asm is true.
        bool emit_asm = false;             ///< Emit assembly text to disk for inspection.
        int optimize = 1;                  ///< Optimization level: 0 = none, 1 = O1, 2 = O2.
        bool skip_il_optimization = false; ///< True when input IL is already optimized.
        bool run_native = false;           ///< Execute the produced binary after linking when true.
        std::size_t stack_size = 0;        ///< Stack size in bytes; 0 means use system default.
        /// Preserve source locations as supported debug data.
        bool emit_debug_lines = false;
        bool time_passes = false; ///< Emit per-backend-pass timings.
        bool verify_mir = false;  ///< Run the MIR verifier after every backend pass.
        bool fast_link = false;   ///< Skip non-essential native-link size reductions.
        /// Name every placed definition in the executable's symbol table so
        /// profilers and debuggers can attribute addresses; false strips them.
        bool emit_local_symbols = true;
        AssemblerMode assembler_mode = AssemblerMode::Native;
        LinkMode link_mode = LinkMode::Native;
        std::string asset_blob_path; ///< Path to ZPAK asset blob for .rodata embedding (optional).
        std::vector<std::string> extra_objects; ///< Extra object files linked into the final image.
        std::optional<bool> windows_debug_runtime; ///< Override Windows CRT import flavor.
        CodegenOptions::TargetABI target_abi = CodegenOptions::TargetABI::Host;
        CodegenOptions::TargetPlatform target_platform = CodegenOptions::TargetPlatform::Host;
    };

    /**
     * @brief Construct a pipeline with an owned option snapshot.
     * @param opts Configuration moved into the new pipeline instance.
     */
    explicit CodegenPipeline(Options opts);

    /**
     * @brief Load the configured IL file and execute the end-to-end workflow.
     * @return Captured output and a zero exit code on success.
     */
    PipelineResult run();

    /**
     * @brief Compile, emit, and optionally execute an already-loaded IL module.
     *
     * Native exception handling is lowered before optional IL optimization.
     * The configured backend passes then produce either assembly or machine
     * code, after which the requested object/link/run actions are performed.
     *
     * @param module IL module consumed by the pipeline.
     * @param debugSourcePath Source path used for generated line-table entries;
     *        defaults to @c Options::input_il_path when empty.
     * @param moduleAlreadyVerified Skip the entry verifier when @c true.
     * @return Captured output and the first failing stage's normalized exit code.
     */
    PipelineResult runWithModule(il::core::Module module,
                                 std::string debugSourcePath = {},
                                 bool moduleAlreadyVerified = false);

  private:
    /// Owned configuration shared by invocations on this instance.
    Options opts_;
};

} // namespace zanna::codegen::x64
