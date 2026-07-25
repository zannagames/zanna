//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/Backend.hpp
// Purpose: Declare the high-level orchestration entry points for x86-64 codegen.
// Key invariants:
//   - emitModuleToAssembly processes all functions in module order.
//   - errors field is empty on success.
//   - Phase A only supports AT&T syntax (atandtSyntax must be true).
// Ownership/Lifetime:
//   - Callers retain ownership of IL modules; generated assembly is returned
//     by value in CodegenResult.
// Links: src/codegen/x86_64/Backend.cpp,
//        src/codegen/x86_64/AsmEmitter.hpp,
//        src/codegen/x86_64/LowerILToMIR.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "AsmEmitter.hpp"
#include "FrameLowering.hpp"
#include "LowerILToMIR.hpp"
#include "codegen/common/objfile/CodeSection.hpp"

#include <string>
#include <vector>

/**
 * @file
 * @brief Declares the public orchestration API for the x86-64 backend.
 *
 * The facade exposes both complete IL-to-output entry points and individual
 * MIR pipeline stages.  Callers that need pass-level control can legalize,
 * allocate, schedule, optimize, and emit explicitly; convenience entry points
 * perform those stages in order.  Failures from pass-oriented APIs are returned
 * through a boolean plus an error string, while invalid-IL legalization in the
 * convenience APIs is reported by exception.
 */

namespace zanna::codegen::x64 {

/**
 * @brief Configures target selection, optimization, and output metadata.
 *
 * A default-constructed instance targets the build host, enables the standard
 * O1 backend pipeline, and emits AT&T assembly without debug line information.
 */
struct CodegenOptions {
    /**
     * @brief Selects the x86-64 calling convention used during lowering.
     */
    enum class TargetABI {
        /// Use the ABI native to the build host.
        Host,
        /// Use the System V AMD64 calling convention.
        SysV,
        /// Use the Microsoft x64 calling convention.
        Win64,
    };

    /**
     * @brief Selects platform-dependent assembly and object-file policy.
     */
    enum class TargetPlatform {
        /// Detect the platform's object format at compile time.
        Host,
        /// Use Darwin/Mach-O conventions.
        Darwin,
        /// Use Linux/ELF conventions.
        Linux,
        /// Use Windows/COFF conventions.
        Windows,
    };

    bool atandtSyntax{true}; ///< Emit AT&T syntax when true; Phase A only supports this form.
    int optimizeLevel{1};    ///< Optimization level: 0 = none, 1 = O1 (default), 2 = O2.
    TargetABI targetABI{TargetABI::Host}; ///< Target ABI used for lowering/allocation.
    TargetPlatform targetPlatform{TargetPlatform::Host}; ///< Object/link/platform policy.
    std::string debugSourcePath{}; ///< Source path used for DWARF line table file entries.
    bool emitDebugLines{false};    ///< Emit DWARF .debug_line entries when true.
};

/**
 * @brief Owns the textual output and diagnostics from assembly emission.
 *
 * @c errors is empty on an unqualified success.  A non-empty value can contain
 * either a non-fatal option warning or an emission failure; on a hard emission
 * failure @c asmText is cleared.
 */
struct CodegenResult {
    std::string asmText{}; ///< Complete assembly text for the requested module/function.
    std::string errors{};  ///< Phase A diagnostics; empty when emission succeeds.
};

/**
 * @brief Run the complete x86-64 assembly pipeline for one IL function.
 *
 * The function is wrapped in a temporary one-function module, legalized,
 * register allocated, scheduled, optimized according to @p opt, and emitted.
 *
 * @param func IL function to translate. The caller retains ownership.
 * @param opt Target and pipeline configuration.
 * @return Owned assembly text and any emission diagnostics.
 * @throws std::runtime_error If IL-to-MIR legalization fails.
 */
[[nodiscard]] CodegenResult emitFunctionToAssembly(const ILFunction &func,
                                                   const CodegenOptions &opt);

/**
 * @brief Run the complete x86-64 assembly pipeline for an IL module.
 *
 * Functions retain module order in the emitted text. Register-allocation,
 * scheduling, optimization, or final-emission failures are returned in the
 * result; legalization failures preserve the legacy throwing contract.
 *
 * @param mod IL module whose functions are translated. The module is not modified.
 * @param opt Target and pipeline configuration.
 * @return Owned module assembly and diagnostics.
 * @throws std::runtime_error If IL-to-MIR legalization fails.
 */
[[nodiscard]] CodegenResult emitModuleToAssembly(const ILModule &mod, const CodegenOptions &opt);

/**
 * @brief Owns code sections and optional DWARF data produced by binary emission.
 *
 * Per-function sections are always retained so downstream object writers can
 * dead-strip functions independently.  When debug lines are requested on a
 * supported format, @c text additionally contains the functions merged in
 * source order and @c debugLineData contains a DWARF v5 line program.
 */
struct BinaryEmitResult {
    objfile::CodeSection text{};   ///< Merged machine code bytes, populated for debug-line output.
    objfile::CodeSection rodata{}; ///< Read-only data (.rodata / __TEXT,__const).
    std::string errors{};          ///< Diagnostics; empty on success.

    /// Per-function text sections for function-level dead stripping.
    /// Each CodeSection contains exactly one function's machine code.
    std::vector<objfile::CodeSection> textSections{};

    /// Pre-encoded DWARF .debug_line bytes (empty when no debug info is available).
    std::vector<uint8_t> debugLineData{};
};

/**
 * @brief Resolve an ABI option to the corresponding immutable target descriptor.
 *
 * @param abi Explicit ABI selection, or @ref CodegenOptions::TargetABI::Host.
 * @return Process-lifetime target information for the selected calling convention.
 */
[[nodiscard]] const TargetInfo &selectTarget(CodegenOptions::TargetABI abi) noexcept;

/**
 * @brief Lower every IL function and run all pre-allocation legalization passes.
 *
 * Functions may be processed concurrently, but their output slots and merged
 * literal-pool labels remain deterministic in module order.  The stage lowers
 * planned calls, performs instruction selection, expands division and overflow
 * pseudos, exposes internal control-flow labels as blocks, and installs the
 * runtime-context initialization sequence for @c main.
 *
 * @param mod Source IL module.
 * @param target ABI and register description used by lowering.
 * @param options Pipeline options reserved for legalization policy.
 * @param roData Destination literal pool. Existing entries are retained.
 * @param mir Destination functions; cleared before work and on failure.
 * @param frames Destination frame summaries parallel to @p mir.
 * @param errors Destination diagnostic; cleared on entry and empty on success.
 * @return @c true on success; @c false if a worker reports an exception.
 */
[[nodiscard]] bool legalizeModuleToMIR(const ILModule &mod,
                                       const TargetInfo &target,
                                       const CodegenOptions &options,
                                       AsmEmitter::RoDataPool &roData,
                                       std::vector<MFunction> &mir,
                                       std::vector<FrameInfo> &frames,
                                       std::string &errors);

/**
 * @brief Allocate registers, assign spill slots, and materialize stack frames.
 *
 * Each MIR function is updated in place, potentially in parallel. Identity
 * register moves left by allocation are removed after all functions complete.
 *
 * @param mir Legalized functions to mutate.
 * @param frames Frame summaries corresponding one-to-one with @p mir.
 * @param target ABI and physical-register description.
 * @param options Backend options forwarded to the per-function pipeline.
 * @param errors Destination diagnostic; cleared on entry and empty on success.
 * @return @c true on success; @c false for a size mismatch or allocation error.
 */
[[nodiscard]] bool allocateModuleMIR(std::vector<MFunction> &mir,
                                     std::vector<FrameInfo> &frames,
                                     const TargetInfo &target,
                                     const CodegenOptions &options,
                                     std::string &errors);

/**
 * @brief Schedule instructions in an allocated MIR module.
 *
 * Scheduling is skipped when @c options.optimizeLevel is less than one.
 *
 * @param mir Allocated functions to schedule in place.
 * @param options Optimization policy controlling whether the pass runs.
 * @param errors Destination diagnostic; cleared on entry and empty on success.
 * @return @c true on success or when skipped; @c false if scheduling throws.
 */
[[nodiscard]] bool scheduleModuleMIR(std::vector<MFunction> &mir,
                                     const CodegenOptions &options,
                                     std::string &errors);

/**
 * @brief Apply target-aware post-allocation peephole optimizations.
 *
 * Functions may be optimized concurrently. The pass is skipped below O1 and
 * resolves its target from @c options.targetABI.
 *
 * @param mir Scheduled functions to optimize in place.
 * @param options Optimization level and target ABI.
 * @param errors Destination diagnostic; cleared on entry and empty on success.
 * @return @c true on success or when skipped; @c false if a peephole pass fails.
 */
[[nodiscard]] bool optimizeModuleMIR(std::vector<MFunction> &mir,
                                     const CodegenOptions &options,
                                     std::string &errors);

/**
 * @brief Serialize a prepared MIR module as assembly text.
 *
 * Functions are emitted in vector order, followed by the read-only literal
 * pool. ELF output also receives a non-executable-stack marker.
 *
 * @param mir Prepared MIR functions.
 * @param roData Literal pool referenced by @p mir.
 * @param target ABI-specific assembly policy.
 * @param options Syntax and target-platform selection.
 * @return Assembly text plus warnings or emission diagnostics.
 */
[[nodiscard]] CodegenResult emitMIRToAssembly(const std::vector<MFunction> &mir,
                                              const AsmEmitter::RoDataPool &roData,
                                              const TargetInfo &target,
                                              const CodegenOptions &options);

/**
 * @brief Encode prepared MIR functions and their literal pool as machine code.
 *
 * Non-debug functions may be encoded concurrently into stable per-function
 * slots. Debug-line emission is serialized so each function's line table can
 * be rebased into the merged text section. COFF output emits Win64 unwind data
 * and currently suppresses DWARF line output.
 *
 * @param mir Prepared MIR functions.
 * @param frames Frame metadata corresponding one-to-one with @p mir.
 * @param roData Literal pool referenced by the functions.
 * @param target ABI and encoding description.
 * @param options Platform and debug-output policy.
 * @return Owned code sections, optional debug data, and diagnostics.
 */
[[nodiscard]] BinaryEmitResult emitMIRToBinary(const std::vector<MFunction> &mir,
                                               const std::vector<FrameInfo> &frames,
                                               const AsmEmitter::RoDataPool &roData,
                                               const TargetInfo &target,
                                               const CodegenOptions &options);

/**
 * @brief Run the complete x86-64 binary pipeline for an IL module.
 *
 * @param mod IL module to lower and encode.
 * @param opt Target, optimization, platform, and debug configuration.
 * @return Per-function machine-code sections, literals, debug data, and errors.
 * @throws std::runtime_error If IL-to-MIR legalization fails.
 */
[[nodiscard]] BinaryEmitResult emitModuleToBinary(const ILModule &mod, const CodegenOptions &opt);

} // namespace zanna::codegen::x64
