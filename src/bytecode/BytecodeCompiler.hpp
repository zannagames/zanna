//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/bytecode/BytecodeCompiler.hpp
// Purpose: Compiles IL modules into compact bytecode for the Zanna bytecode VM.
// Key invariants: SSA values are deterministically mapped to local slots.
//                 Block linearization preserves fall-through for the common case.
//                 All branch offsets are resolved before the function is finalized.
// Ownership: Produces BytecodeModule instances; does not take ownership of input
//            IL modules.
// Lifetime: Compiler state is transient per compile() call; the resulting
//           BytecodeModule outlives the compiler.
// Links: Bytecode.hpp, BytecodeModule.hpp, il/core/Module.hpp
//
//===----------------------------------------------------------------------===//
//
// This file defines the BytecodeCompiler which transforms IL modules into
// compact bytecode for fast interpretation. The compiler performs:
// - SSA to locals mapping
// - Block linearization
// - Constant pool building
// - Bytecode instruction emission
// - Branch offset resolution

/**
 * @file src/bytecode/BytecodeCompiler.hpp
 * @brief Declares the stateful IL-to-bytecode lowering pipeline.
 *
 * @details
 * One compiler instance owns a `BytecodeModule` under construction plus
 * per-function SSA/local, block/fixup, source-file, stack-depth, and allocation
 * accounting. Input IL and source-manager state is borrowed only while a
 * compile entry point is active; successful results own all data required by
 * the bytecode VM.
 */

#pragma once

#include "bytecode/Bytecode.hpp"
#include "bytecode/BytecodeModule.hpp"
#include "il/core/Module.hpp"
#include "support/diag_expected.hpp"
#include "support/source_location.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zanna {
namespace bytecode {

/**
 * @brief Transforms IL modules into self-contained bytecode modules.
 *
 * The compiler optionally verifies its input, assigns globals and function
 * indices, maps function-scoped SSA values to locals, linearizes blocks, builds
 * constant pools, emits stack bytecode, resolves control-flow fixups, and
 * derives exception/switch metadata.
 *
 * @invariant `currentStackDepth_` mirrors the stack effect of emitted code.
 * @invariant No function is finalized with unresolved branch placeholders.
 * @ownership The compiler owns transient and output state while borrowing the
 *            current IL module and optional source manager.
 */
class BytecodeCompiler {
  public:
    /// @brief Compile an entire IL module to a bytecode module.
    /// @details Iterates over all functions in the IL module, compiles each
    ///          one into a BytecodeFunction, builds the shared constant pools
    ///          (i64, f64, string), and assembles the result into a
    ///          BytecodeModule with resolved branch offsets.
    /// @param ilModule Borrowed IL module to verify and lower.
    /// @return An owning `BytecodeModule` ready for execution.
    /// @throws std::runtime_error When checked compilation returns a diagnostic.
    BytecodeModule compile(const il::core::Module &ilModule);

    /**
     * @brief Compile an IL module and report expected failures diagnostically.
     * @param ilModule Borrowed IL module to lower.
     * @param sourceManager Optional borrowed resolver for source paths stored in
     *                      debug metadata; it is not retained after compilation.
     * @param assumeVerified Skip verifier preflight only when the caller has
     *                       already verified this exact module snapshot.
     * @return An owning bytecode module on success or one structured diagnostic
     *         on verification, lowering, or internal failure.
     */
    il::support::Expected<BytecodeModule> compileChecked(
        const il::core::Module &ilModule,
        const il::support::SourceManager *sourceManager = nullptr,
        bool assumeVerified = false);

  private:
    /// @brief The bytecode module being built during compilation.
    BytecodeModule module_;

    /// @brief The IL module being compiled (used for global lookups).
    const il::core::Module *ilModule_ = nullptr;

    /// @brief Pointer to the BytecodeFunction currently being compiled.
    BytecodeFunction *currentFunc_ = nullptr;

    /// @brief Name of the function currently being compiled.
    std::string currentFunctionName_;

    /// @brief Source location of the instruction currently being compiled.
    il::support::SourceLoc currentLoc_{};

    /// @brief Block label of the instruction currently being compiled.
    std::string currentBlockLabel_;

    /// @brief Source manager used to resolve file ids for bytecode debug metadata.
    const il::support::SourceManager *sourceManager_{nullptr};

    /// @brief Mapping from SourceManager file identifiers to BytecodeModule source indices.
    std::unordered_map<uint32_t, uint32_t> sourceFileIndex_;

    /// @brief Mapping from SSA value IDs to local variable slot indices.
    /// @details Populated per-function by buildSSAToLocalsMap().
    std::unordered_map<uint32_t, uint32_t> ssaToLocal_;

    /// @brief Next available local slot index for the current function.
    uint32_t nextLocal_ = 0;

    /// @brief Per-local string classification for the current function.
    /// @details Indexed by bytecode local slot. A value of 1 means the slot
    ///          stores a managed runtime string handle and therefore needs
    ///          retain/release semantics in the bytecode VM.
    std::vector<uint8_t> localIsString_;

    /// @brief Mapping from block labels to their bytecode offsets.
    /// @details Populated during block emission and consumed during branch fixup.
    std::unordered_map<std::string, uint32_t> blockOffsets_;

    /// @brief Mapping from block labels to the SSA IDs of their block parameters.
    /// @details Used to emit stores for branch arguments that feed block parameters.
    std::unordered_map<std::string, std::vector<uint32_t>> blockParamIds_;

    /// @brief Set of block labels that are direct targets of eh.push instructions.
    /// @details Only these blocks receive handler values (error, resume_token)
    ///          via the dispatchTrap stack push. Other blocks with eh.entry (e.g.,
    ///          typed-catch forwarding blocks) receive values via normal branch args.
    std::unordered_set<std::string> ehPushTargets_;

    /// @brief A pending branch fixup requiring offset resolution after all blocks
    ///        have been emitted.
    struct BranchFixup {
        uint32_t codeOffset = 0; ///< Index into the code vector where the offset is stored.
        std::string targetLabel; ///< Target block label to resolve.
        bool isLong = false;     ///< True if the offset is 24-bit; false for 16-bit.
        bool isRaw = false; ///< True if the offset is stored as a raw i32 (not encoded in opcode).
        il::support::SourceLoc loc{}; ///< Source location of the branch instruction.
    };

    /// @brief List of branch fixups accumulated during function compilation.
    std::vector<BranchFixup> pendingBranches_;

    /// @brief Current operand stack depth during emission (for max stack calculation).
    int32_t currentStackDepth_ = 0;

    /// @brief Maximum operand stack depth observed during compilation of the current function.
    int32_t maxStackDepth_ = 0;

    /// @brief Maximum statically known alloca usage in the current function.
    /// @details Constant alloca sizes are summed conservatively into this
    ///          field. A dynamic size records the VM's current alloca arena cap
    ///          so consumers know the function may use the full runtime budget.
    uint32_t maxAllocaSize_ = 0;

    /// @brief Compile a single IL function into a BytecodeFunction.
    /// @details Builds the SSA-to-locals map, linearizes blocks, emits bytecode
    ///          for each block, and resolves branch fixups.
    /// @param fn The IL function to compile.
    void compileFunction(const il::core::Function &fn);

    /// @brief Register IL globals in the bytecode module before function lowering.
    /// @param module The source IL module.
    void registerGlobals(const il::core::Module &module);

    /// @brief Emit a pointer to bytecode global storage.
    /// @param name Global name to resolve.
    /// @param loc Source location for diagnostics.
    void emitGlobalAddress(std::string_view name, il::support::SourceLoc loc);

    /// @brief Build the SSA value ID to local slot mapping for a function.
    /// @details Assigns each SSA value (parameters, instruction results) a
    ///          unique local slot index. Parameters occupy the first N slots.
    /// @param fn The IL function whose SSA values are to be mapped.
    void buildSSAToLocalsMap(const il::core::Function &fn);

    /// @brief Linearize basic blocks into an ordered sequence for emission.
    /// @details Orders blocks so that the most likely fall-through successor
    ///          immediately follows its predecessor, minimizing jump instructions.
    /// @param fn The IL function whose blocks are to be linearized.
    /// @return An ordered vector of basic block pointers for sequential emission.
    static std::vector<const il::core::BasicBlock *> linearizeBlocks(const il::core::Function &fn);

    /// @brief Compile all instructions in a basic block.
    /// @details Records the block's bytecode offset, emits code for each
    ///          instruction, and handles block parameter stores.
    /// @param block The basic block to compile.
    void compileBlock(const il::core::BasicBlock &block);

    /// @brief Compile a single IL instruction into one or more bytecode instructions.
    /// @param instr The IL instruction to compile.
    void compileInstr(const il::core::Instr &instr);

    /**
     * @brief Terminate lowering with a structured diagnostic.
     * @param loc Source location attached to the error.
     * @param code Stable diagnostic code.
     * @param message Human-readable detail augmented with function context.
     * @throws BytecodeCompileFailure Always; the private implementation type is
     *         caught by `compileChecked()`.
     */
    [[noreturn]] void fail(il::support::SourceLoc loc, std::string code, std::string message) const;

    /**
     * @brief Terminate lowering at `currentLoc_`.
     * @param code Stable diagnostic code.
     * @param message Human-readable detail.
     * @throws BytecodeCompileFailure Always.
     */
    [[noreturn]] void failCurrent(std::string code, std::string message) const;

    /**
     * @brief Require a minimum operand count on a lowering input.
     * @param instr Borrowed instruction to inspect.
     * @param minCount Required lower bound.
     * @throws BytecodeCompileFailure When the instruction has fewer operands.
     */
    void requireOperandCount(const il::core::Instr &instr, size_t minCount) const;

    /// @brief Emit bytecode to push an IL value onto the operand stack.
    /// @details Handles constants (immediates), SSA references (local loads),
    ///          and global references.
    /// @param val The IL value to push.
    void pushValue(const il::core::Value &val);

    /// @brief Pop TOS and store the result into the local slot for the instruction's SSA ID.
    /// @param instr The IL instruction whose result is being stored.
    void storeResult(const il::core::Instr &instr);

    /// @brief Emit a raw 32-bit instruction word into the current function's code.
    /// @param instr The pre-encoded 32-bit instruction word.
    void emit(uint32_t instr);

    /// @brief Intern @p loc's source file; returns a 1-based table index
    ///        (0 = no file).
    /// @param loc Source location whose file identifier is interned.
    /// @return One-based module source-file entry, or zero without a file id.
    uint32_t sourceFileTableEntry(il::support::SourceLoc loc);

    /// @brief Emit a zero-argument bytecode instruction.
    /// @param op The opcode to emit.
    void emit(BCOpcode op);

    /// @brief Emit a bytecode instruction with one unsigned 8-bit argument.
    /// @param op  The opcode to emit.
    /// @param arg The 8-bit unsigned argument.
    void emit8(BCOpcode op, uint8_t arg);

    /// @brief Emit a bytecode instruction with one signed 8-bit argument.
    /// @param op  The opcode to emit.
    /// @param arg The signed 8-bit argument.
    void emitI8(BCOpcode op, int8_t arg);

    /// @brief Emit a bytecode instruction with one unsigned 16-bit argument.
    /// @param op  The opcode to emit.
    /// @param arg The 16-bit unsigned argument.
    void emit16(BCOpcode op, uint16_t arg);

    /// @brief Emit a bytecode instruction with one signed 16-bit argument.
    /// @param op  The opcode to emit.
    /// @param arg The signed 16-bit argument.
    void emitI16(BCOpcode op, int16_t arg);

    /// @brief Emit a constant-pool load (16-bit @p index); fails if the pool
    ///        named @p poolName exceeds 65535 entries.
    /// @param op Constant-loading opcode.
    /// @param index Zero-based pool index.
    /// @param poolName Borrowed pool label used in diagnostics.
    void emitPoolLoad(BCOpcode op, uint32_t index, std::string_view poolName);

    /// @brief Emit a bytecode instruction with two unsigned 8-bit arguments.
    /// @param op   The opcode to emit.
    /// @param arg0 First 8-bit unsigned argument.
    /// @param arg1 Second 8-bit unsigned argument.
    void emit88(BCOpcode op, uint8_t arg0, uint8_t arg1);

    /// @brief Emit a branch instruction with a pending fixup for the target label.
    /// @details The target offset is left as a placeholder and resolved later
    ///          by resolveBranches(). Uses a 16-bit offset encoding.
    /// @param op    The branch opcode (JUMP, JUMP_IF_TRUE, JUMP_IF_FALSE).
    /// @param label The target basic block label.
    void emitBranch(BCOpcode op, const std::string &label);

    /// @brief Emit a long branch instruction with a pending fixup for the target label.
    /// @details Uses a 24-bit offset encoding for blocks farther than 16-bit range.
    /// @param op    The branch opcode (JUMP_LONG).
    /// @param label The target basic block label.
    void emitBranchLong(BCOpcode op, const std::string &label);

    /// @brief Resolve all pending branch fixups by patching target offsets.
    /// @details Called once after all blocks have been emitted. Computes the
    ///          signed offset from the branch instruction to the target block
    ///          and encodes it into the previously emitted placeholder.
    void resolveBranches();

    /// @brief Rebuild function metadata derived from the emitted bytecode.
    /// @details Scans the finalized instruction stream after branch offsets
    ///          have been resolved and populates switch table metadata and
    ///          exception handler ranges. This keeps the metadata vectors in
    ///          sync with the executable bytecode without changing instruction
    ///          encoding.
    void rebuildDerivedMetadata();

    /// @brief Record a compile-time alloca size contribution.
    /// @details Constant non-negative sizes are aligned to the VM's alloca
    ///          granularity and contribute to @ref maxAllocaSize_. Dynamic
    ///          sizes conservatively mark the function as potentially using
    ///          the full alloca arena.
    /// @param sizeOperand Operand passed to the IL alloca instruction.
    void recordAllocaSize(const il::core::Value &sizeOperand);

    /// @brief Record that the operand stack grows by @p count entries.
    /// @details Updates currentStackDepth_ and maxStackDepth_ for accurate
    ///          max-stack calculation in the compiled function.
    /// @param count Number of stack entries pushed (default 1).
    void pushStack(int32_t count = 1);

    /// @brief Record that the operand stack shrinks by @p count entries.
    /// @param count Number of stack entries popped (default 1).
    void popStack(int32_t count = 1);

    /// @brief Get a local variable slot for an SSA value ID.
    /// @details Returns the existing mapping. Unknown SSA IDs are malformed IL
    ///          and are reported as diagnostics instead of becoming implicit locals.
    /// @param ssaId The SSA value identifier.
    /// @return The local slot index assigned to this SSA value.
    uint32_t getLocal(uint32_t ssaId);

    /// @brief Emit a LOAD_LOCAL or LOAD_LOCAL_W instruction based on slot index size.
    /// @details Selects the narrow (8-bit index) or wide (16-bit index) variant
    ///          automatically based on whether the index fits in 8 bits.
    /// @param local The local variable slot index to load.
    void emitLoadLocal(uint32_t local);

    /// @brief Emit a STORE_LOCAL or STORE_LOCAL_W instruction based on slot index size.
    /// @details Selects the narrow (8-bit index) or wide (16-bit index) variant
    ///          automatically based on whether the index fits in 8 bits.
    /// @param local The local variable slot index to store to.
    void emitStoreLocal(uint32_t local);

    /// @brief Compile an IL arithmetic instruction into corresponding bytecode.
    /// @param instr The IL arithmetic instruction (add, sub, mul, div, rem, neg).
    void compileArithmetic(const il::core::Instr &instr);

    /// @brief Compile an IL comparison instruction into corresponding bytecode.
    /// @param instr The IL comparison instruction (eq, ne, lt, le, gt, ge).
    void compileComparison(const il::core::Instr &instr);

    /// @brief Compile an IL type conversion instruction into corresponding bytecode.
    /// @param instr The IL conversion instruction (i64_to_f64, f64_to_i64, etc.).
    void compileConversion(const il::core::Instr &instr);

    /// @brief Compile an IL bitwise operation instruction into corresponding bytecode.
    /// @param instr The IL bitwise instruction (and, or, xor, not, shl, shr).
    void compileBitwise(const il::core::Instr &instr);

    /// @brief Compile an IL memory operation instruction into corresponding bytecode.
    /// @param instr The IL memory instruction (alloca, gep, load, store).
    void compileMemory(const il::core::Instr &instr);

    /// @brief Compile an IL call instruction into corresponding bytecode.
    /// @param instr The IL call instruction (direct, native, or indirect).
    void compileCall(const il::core::Instr &instr);

    /// @brief Compile an IL branch instruction into corresponding bytecode.
    /// @param instr The IL branch instruction (unconditional, conditional, switch).
    void compileBranch(const il::core::Instr &instr);

    /// @brief Compile an IL return instruction into corresponding bytecode.
    /// @param instr The IL return instruction (return value or return void).
    void compileReturn(const il::core::Instr &instr);
};

} // namespace bytecode
} // namespace zanna
