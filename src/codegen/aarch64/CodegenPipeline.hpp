//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/CodegenPipeline.hpp
// Purpose: Entry point for the modular AArch64 code-generation pipeline.
//          Wires together all AArch64 passes via the PassManager and
//          replaces the monolithic per-function loop in the CLI driver.
//
// Key invariants:
//   - Passes run in order: Lowering → Legalize → optional PreRegAllocOpt →
//     RegAlloc → BlockLayout → Peephole → Scheduler → Peephole → Emit.
//   - The AArch64Module struct is threaded through all passes.
//   - MIR dump callbacks fire between passes when enabled.
//
// Ownership/Lifetime:
//   - The pipeline operates on a caller-owned AArch64Module reference.
//   - The pipeline does not own the IL module or TargetInfo.
//
// Links: src/codegen/aarch64/passes/PassManager.hpp (types),
//        src/tools/zanna/cmd_codegen_arm64.cpp (caller)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares pass-level and end-to-end AArch64 code-generation drivers.
 *
 * `runCodegenPipeline` operates on a prepared module and runs MIR passes.
 * `CodegenPipeline` additionally owns CLI-style options and orchestrates module
 * loading, IL verification/optimization, object emission, linking, and optional
 * execution while capturing user-visible output.
 */

#pragma once

#include "codegen/aarch64/passes/PassManager.hpp"

#include <cstddef>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace zanna::codegen::aarch64 {

/// @brief Result returned by a pipeline run.
struct PipelineResult {
    int exit_code{0};          ///< 0 on success, non-zero on error.
    std::string stdout_text{}; ///< Captured stdout (assembly text when emit_asm is set).
    std::string stderr_text{}; ///< Captured stderr (diagnostics, MIR dumps).
};

/// @brief Options controlling optional MIR dumps and diagnostics for the
///        pass-level pipeline (used by runCodegenPipeline).
struct PipelineOptions {
    bool dumpMirBeforeRA = false; ///< Dump MIR to diagOut before register allocation.
    bool dumpMirAfterRA = false;  ///< Dump MIR to diagOut after register allocation.
    bool emitAssemblyText = true; ///< Run EmitPass to produce assembly text output.
    bool useBinaryEmit = false;   ///< Also run BinaryEmitPass to produce an object file.
    int optimizeLevel = 1;        ///< Backend optimization level: 0 = none, 1 = peephole+scheduler.
    bool timePasses = false;      ///< Emit per-pass wall-clock timings to diagOut.
};

/// @brief High-level driver for the AArch64 code-generation pipeline.
///
/// Wraps the full AArch64 pipeline (IL parse → IL opt → MIR lower → RA →
/// peephole → sched → emit → assemble → link) behind a single `run()` entry
/// point used by the `zanna` CLI driver.
/// @invariant `opts_` is immutable configuration from the caller's perspective
///            for the duration of each run.
class CodegenPipeline {
  public:
    /// @brief Which assembler to use for translating emitted .s files to .o files.
    enum class AssemblerMode {
        System, ///< Invoke the system `as` (or clang -c) via subprocess.
        Native, ///< Use Zanna's built-in assembler/encoder.
    };

    /// @brief Which linker to use for producing the final native binary.
    enum class LinkMode {
        System, ///< Invoke the system `ld` (or clang) via subprocess.
        Native, ///< Use Zanna's built-in linker.
    };

    /// @brief OS target for emitted code; affects symbol mangling and ABI.
    enum class TargetPlatform {
        Host,    ///< Auto-detect from the current OS at runtime.
        Darwin,  ///< macOS / iOS (Mach-O, underscore symbol prefix).
        Linux,   ///< Linux ELF (no prefix, .type/.size directives).
        Windows, ///< Windows PE/COFF (no prefix, no .type/.size).
    };

    /// @brief All configuration for a single pipeline run.
    struct Options {
        std::string input_il_path{};       ///< Path to the IL module file (.il) to compile.
        std::string output_obj_path{};     ///< Path to write the output .o file.
        std::string output_asm_path{};     ///< Path to write the assembly text (if emit_asm).
        bool emit_asm = false;             ///< Also write assembly text to output_asm_path.
        bool run_native = false;           ///< Execute the compiled binary after linking.
        bool dump_mir_before_ra = false;   ///< Dump MIR to stderr before register allocation.
        bool dump_mir_after_ra = false;    ///< Dump MIR to stderr after register allocation.
        int optimize = 1;                  ///< 0 = no backend opts; 1 = peephole + scheduler.
        bool skip_il_optimization = false; ///< Skip IL optimizer passes (for testing).
        std::size_t stack_size = 0;        ///< Requested stack size in bytes; 0 = platform default.
        AssemblerMode assembler_mode = AssemblerMode::Native;  ///< Assembler selection.
        LinkMode link_mode = LinkMode::Native;                 ///< Linker selection.
        TargetPlatform target_platform = TargetPlatform::Host; ///< OS ABI target.
        bool emit_debug_lines = false; ///< Emit .loc / line-number directives in assembly.
        bool time_passes = false;      ///< Print per-pass wall-clock timings to stderr.
        bool fast_link = false;        ///< Skip non-essential size-reduction passes in the linker.
        /// Name every placed definition in the executable's symbol table so
        /// profilers and debuggers can attribute addresses; false strips them.
        bool emit_local_symbols = true;
        std::string asset_blob_path{}; ///< Path to ZPAK asset blob for .rodata embedding.
        std::vector<std::string> extra_objects{};    ///< Extra .o files to pass to the linker.
        std::optional<bool> windows_debug_runtime{}; ///< Override Windows CRT import flavor.
    };

    /// @brief Construct a pipeline with the given options.
    /// @param opts Configuration moved into the pipeline.
    explicit CodegenPipeline(Options opts);

    /// @brief Run the pipeline reading the IL module from Options::input_il_path.
    /// @return PipelineResult with exit_code 0 on success.
    [[nodiscard]] PipelineResult run();

    /// @brief Run the pipeline using an already-loaded IL module.
    /// @param module The IL module to compile (consumed).
    /// @param debugSourcePath Source path string embedded in debug info (optional).
    /// @param moduleAlreadyVerified Skip IL verification when true.
    /// @return Captured output and a zero exit code on success.
    [[nodiscard]] PipelineResult runWithModule(il::core::Module module,
                                               std::string debugSourcePath = {},
                                               bool moduleAlreadyVerified = false);

  private:
    Options opts_; ///< Pipeline configuration captured at construction time.
};

/// @brief Run the full AArch64 code-generation pipeline.
/// @param module  Mutable module state; ilMod and ti must be set.
/// @param opts    Pipeline options (MIR dump flags, etc.).
/// @param diagOut Stream receiving MIR dumps and diagnostics.
/// @return true on success, false on error.
bool runCodegenPipeline(passes::AArch64Module &module,
                        const PipelineOptions &opts,
                        std::ostream &diagOut);

} // namespace zanna::codegen::aarch64
