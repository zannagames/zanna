//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares the FunctionVerifier class, which orchestrates comprehensive
// validation of IL function definitions. The FunctionVerifier is the central
// coordinator for verifying function bodies, dispatching to specialized strategies
// for different instruction categories.
//
// Functions are the primary unit of code in IL modules. Import declarations have
// no blocks; defined functions contain one or more blocks whose instructions are
// structurally and semantically verified. The FunctionVerifier ensures that every
// function adheres to IL specification rules including proper control flow
// structure, type safety, and exception handling semantics.
//
// Key Responsibilities:
// - Validate function signature (name, parameters, return type)
// - Build basic block symbol tables for control flow edge validation
// - Verify basic block structure (parameters, terminator presence)
// - Dispatch instruction verification to registered strategies
// - Maintain type inference context across instruction verification
// - Validate exception handler blocks and their signatures
//
// Design Rationale:
// The strategy pattern enables extensible instruction verification. Each opcode
// category (arithmetic, memory, control flow, exceptions) has dedicated strategy
// implementations. The FunctionVerifier coordinates strategy execution while
// maintaining the shared verification context (type environment, block maps,
// extern/function tables) needed by all strategies.
//
// Ownership and Lifetime:
// The FunctionVerifier retains references to caller-owned extern and global maps,
// which must outlive it. Function pointers indexed during run() borrow the module
// and are used only for that invocation. Per-function block maps and type
// environments remain local to verification.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares module-wide coordination of IL function verification.
/// @details `FunctionVerifier` validates names and signatures, block structure,
///          SSA typing and dominance, effect promises, ownership-release
///          discipline, stack-pointer escape rules, and handler-local semantics
///          through opcode strategies.

#pragma once

#include "il/verify/BlockMap.hpp"
#include "il/verify/DiagSink.hpp"
#include "il/verify/ExceptionHandlerAnalysis.hpp"

#include "support/diag_expected.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace il::core {
struct Module;
struct Function;
struct BasicBlock;
struct Instr;
struct Extern;
struct Global;
struct Type;
} // namespace il::core

namespace il::verify {

class TypeInference;

/**
 * @brief Validates IL function semantics and type safety.
 *
 * The FunctionVerifier is the central component of the IL verification pipeline.
 * It ensures that every function in a module adheres to the IL specification,
 * including proper typing, valid control flow, and correct SSA form.
 *
 * ## Verification Phases
 *
 * For each function, the verifier executes these phases in order:
 *
 * 1. **Signature Validation**: Verify function name, parameter types, and return type
 *    conform to IL type system rules.
 *
 * 2. **Block Map Construction**: Build a symbol table mapping block labels to their
 *    definitions for efficient control flow edge resolution.
 *
 * 3. **Block Structure Validation**: Ensure each basic block has:
 *    - A unique label within the function
 *    - Properly typed block parameters (for phi-like semantics)
 *    - Exactly one terminator instruction at the end
 *
 * 4. **Instruction Verification**: For each instruction:
 *    - Dispatch to the appropriate InstructionStrategy based on opcode
 *    - Perform type inference for the result (if any)
 *    - Validate operand types against the instruction's signature
 *    - Check SSA constraints (single definition, use after def)
 *
 * 5. **Exception Handler Validation**: For functions with exception handling:
 *    - Verify eh.push/eh.pop balance
 *    - Ensure handler blocks have correct entry signatures
 *    - Validate resume instructions target valid handler contexts
 *
 * ## Strategy Pattern
 *
 * Instruction verification uses the strategy pattern for extensibility. Each opcode
 * category (arithmetic, memory, control flow, exceptions) has a dedicated strategy
 * implementation. The FunctionVerifier coordinates strategy execution while maintaining
 * the shared context (type environment, block maps) needed by all strategies.
 *
 * ## Error Handling
 *
 * The verifier uses the Expected<void> pattern for error reporting:
 * - Returns success if all checks pass
 * - Returns an error with a detailed diagnostic message on first failure
 * - Non-fatal warnings are collected in the DiagSink for later reporting
 *
 * @invariant All functions in a module must pass verification before execution.
 * @invariant The verifier does not modify the module being verified.
 *
 * @see InstructionStrategy for the strategy interface
 * @see TypeInference for the type checking context
 * @see BlockMap for control flow graph representation
 */
class FunctionVerifier {
  public:
    /// @brief Map from extern function names to their declarations.
    ///
    /// Used to resolve external function calls during verification. The verifier
    /// checks that call instructions to external functions match the declared
    /// signature (parameter count and types, return type).
    using ExternMap = std::unordered_map<std::string, const il::core::Extern *>;

    /// @brief Map from global symbol names to borrowed declarations.
    /// @details Used to validate global-address operands and detect collisions
    ///          with functions and externs.
    using GlobalMap = std::unordered_map<std::string, const il::core::Global *>;

    /// @brief Constructs a function verifier with access to external declarations.
    ///
    /// The extern map is stored by reference and must remain valid for the lifetime
    /// of the verifier. Typically this map is built from the module's extern table
    /// before constructing the verifier.
    ///
    /// @param externs Map of external function declarations for call validation.
    explicit FunctionVerifier(const ExternMap &externs, const GlobalMap &globals);

    /**
     * @brief Verify all functions in the module for correctness.
     *
     * Applies instruction-specific verification strategies to validate:
     * - Type consistency across operations
     * - Proper SSA form with unique definitions
     * - Valid control flow targets
     * - Correct external function usage
     *
     * @param module Module containing functions to verify.
     * @param sink Collector for non-fatal warnings during verification.
     * @return Success if all functions pass, or the first function diagnostic.
     *         Per-function failures are also reported to @p sink while later
     *         independent functions continue to be checked.
     */
    [[nodiscard]] il::support::Expected<void> run(const il::core::Module &module, DiagSink &sink);

    /**
     * @brief Strategy interface for opcode-specific instruction verification.
     *
     * Each instruction category (arithmetic, memory, control flow, exceptions, etc.)
     * has a dedicated strategy implementation that knows how to verify instructions
     * of that category. The FunctionVerifier maintains a list of strategies and
     * dispatches each instruction to the first matching strategy.
     *
     * ## Implementing a Strategy
     *
     * To add verification for a new instruction category:
     *
     * 1. Create a class derived from InstructionStrategy
     * 2. Implement matches() to return true for handled opcodes
     * 3. Implement verify() to perform category-specific validation
     * 4. Register the strategy in FunctionVerifier's constructor
     *
     * ## Strategy Priority
     *
     * Strategies are checked in registration order. The first strategy where
     * matches() returns true handles the instruction. This allows general fallback
     * strategies to be registered last.
     *
     * ## Thread Safety
     *
     * Strategy instances are shared across all function verifications and must be
     * stateless (const methods only) for thread safety.
     *
     * @see ArithmeticStrategy for an example implementation
     * @see ControlFlowStrategy for branching instruction verification
     */
    class InstructionStrategy {
      public:
        /// @brief Virtual destructor for proper polymorphic cleanup.
        virtual ~InstructionStrategy() = default;

        /// @brief Tests whether this strategy handles the given instruction.
        ///
        /// Called by the FunctionVerifier to find the appropriate strategy for
        /// each instruction. Should be a fast check, typically just comparing
        /// the opcode against a known set.
        ///
        /// @param instr The instruction to test.
        /// @return True if this strategy can verify the instruction, false otherwise.
        virtual bool matches(const il::core::Instr &instr) const = 0;

        /// @brief Verifies an instruction for correctness.
        ///
        /// Performs all verification checks specific to this instruction category:
        /// - Operand type validation against the opcode's signature
        /// - Result type inference and registration with the type environment
        /// - Control flow target resolution (for branches/calls)
        /// - Side effect tracking and ordering constraints
        ///
        /// @param fn The containing function (for context in diagnostics).
        /// @param bb The containing basic block.
        /// @param instr The instruction to verify.
        /// @param blockMap Symbol table for resolving branch targets.
        /// @param externs External function declarations for call validation.
        /// @param funcs Module-internal function declarations for call validation.
        /// @param types Type inference context for operand/result type checking.
        /// @param sink Collector for non-fatal warnings.
        /// @return Success if verification passes, error with diagnostic on failure.
        [[nodiscard]] virtual il::support::Expected<void> verify(
            const il::core::Function &fn,
            const il::core::BasicBlock &bb,
            const il::core::Instr &instr,
            const BlockMap &blockMap,
            const std::unordered_map<std::string, const il::core::Extern *> &externs,
            const std::unordered_map<std::string, const il::core::Function *> &funcs,
            const std::unordered_map<std::string, const il::core::Global *> &globals,
            TypeInference &types,
            DiagSink &sink) const = 0;
    };

  private:
    /// @brief Verifies a single function for correctness.
    ///
    /// Entry point for per-function verification. Builds the block map, initializes
    /// the type inference context, and iterates over all basic blocks calling
    /// verifyBlock() for each.
    ///
    /// @param fn The function to verify.
    /// @param sink Collector for non-fatal warnings.
    /// @return Success if the function passes all checks, error on first failure.
    il::support::Expected<void> verifyFunction(const il::core::Function &fn, DiagSink &sink);

    /// @brief Verify SSA dominance (every temp use dominated by its def) and
    ///        alloca-escape rules — PASS 3 and PASS 4 of verifyFunction.
    /// @details Also checks runtime-handle release ordering with both dominance
    ///          and forward data-flow analyses.
    /// @param fn Function whose control flow and uses are inspected.
    /// @param blockMap Resolved label map for CFG construction.
    /// @param temps Pre-collected temp id → type map from PASS 1.
    /// @param definingBlock Defining block for each instruction or block-param
    ///        temporary; function parameters are absent.
    /// @return Success when dominance, release, and escape constraints hold;
    ///         otherwise the first diagnostic.
    il::support::Expected<void> verifyDominanceAndEscapes(
        const il::core::Function &fn,
        const BlockMap &blockMap,
        const std::unordered_map<unsigned, il::core::Type> &temps,
        const std::unordered_map<unsigned, const il::core::BasicBlock *> &definingBlock);

    /// @brief Verifies a single basic block for correctness.
    ///
    /// Validates block structure (parameters, terminator) and iterates over all
    /// instructions calling verifyInstruction() for each. Tracks SSA definition
    /// points to ensure single-definition rule is maintained.
    ///
    /// @param fn The containing function (for context in diagnostics).
    /// @param bb The basic block to verify.
    /// @param blockMap Symbol table for resolving branch targets.
    /// @param temps Map from temporary indices to their inferred types.
    /// @param definingBlock Map tracking which block defines each temporary.
    /// @param sink Collector for non-fatal warnings.
    /// @return Success if the block passes all checks, error on first failure.
    il::support::Expected<void> verifyBlock(
        const il::core::Function &fn,
        const il::core::BasicBlock &bb,
        const BlockMap &blockMap,
        std::unordered_map<unsigned, il::core::Type> &temps,
        const std::unordered_map<unsigned, const il::core::BasicBlock *> &definingBlock,
        DiagSink &sink);

    /// @brief Verifies a single instruction for correctness.
    ///
    /// Dispatches to the appropriate InstructionStrategy based on the instruction's
    /// opcode. Falls back to a generic error if no strategy handles the instruction.
    ///
    /// @param fn The containing function (for context in diagnostics).
    /// @param bb The containing basic block.
    /// @param instr The instruction to verify.
    /// @param blockMap Symbol table for resolving branch targets.
    /// @param types Type inference context for operand/result type checking.
    /// @param sink Collector for non-fatal warnings.
    /// @return Success if the instruction passes verification, error on failure.
    il::support::Expected<void> verifyInstruction(const il::core::Function &fn,
                                                  const il::core::BasicBlock &bb,
                                                  const il::core::Instr &instr,
                                                  const BlockMap &blockMap,
                                                  TypeInference &types,
                                                  DiagSink &sink);

    /// @brief Formats a diagnostic message with function context.
    ///
    /// Prepends function name and location information to a diagnostic message
    /// for clearer error reporting.
    ///
    /// @param fn The function providing context.
    /// @param message The base diagnostic message.
    /// @return Formatted message with function context prefix.
    [[nodiscard]] std::string formatFunctionDiag(const il::core::Function &fn,
                                                 std::string_view message) const;

    /// @brief Reference to the module's external function declarations.
    ///
    /// Used to resolve and validate calls to external functions. The map is
    /// owned by the caller and must remain valid for the verifier's lifetime.
    const ExternMap &externs_;

    /// @brief Reference to the module's global declarations.
    ///
    /// Used to validate global-address operands for const strings and mutable
    /// storage references.
    const GlobalMap &globals_;

    /// @brief Map from function names to their definitions within the module.
    ///
    /// Built during run() from the module's function list. Used to resolve
    /// direct function calls to module-internal functions and validate their
    /// signatures match the call site.
    std::unordered_map<std::string, const il::core::Function *> functionMap_;

    /// @brief Map from exception handler labels to their expected entry signatures.
    ///
    /// Populated during exception handler analysis. When an eh.push instruction
    /// is encountered, the handler block's expected parameter types are recorded
    /// here for validation when the handler block is verified.
    std::unordered_map<std::string, HandlerSignature> handlerInfo_;

    /// @brief Registered instruction verification strategies.
    ///
    /// Each strategy handles a category of opcodes (arithmetic, memory, control flow,
    /// etc.). Strategies are checked in order; the first matching strategy verifies
    /// the instruction. Built during FunctionVerifier construction.
    std::vector<std::unique_ptr<InstructionStrategy>> strategies_;
};

} // namespace il::verify
