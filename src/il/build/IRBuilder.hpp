//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief High-level API for constructing IL modules programmatically.
 *
 * IRBuilder is the primary interface used by frontends to generate IL code from
 * source languages. It maintains an insertion point (current basic block) and
 * provides fluent helpers to emit instructions, manage control flow, and track
 * SSA temporaries. It enforces structural invariants (e.g., one terminator per
 * block) and simplifies common patterns like creating branches, calls, and
 * arithmetic operations.
 *
 * @section capabilities Key Capabilities
 * - Module construction: add externs, globals, and functions
 * - Block management: create blocks, set insertion points, track terminators
 * - Instruction emission: arithmetic, comparisons, memory ops, control flow
 * - SSA management: automatic temporary ID assignment and tracking
 * - Type safety: type-aware instruction constructors
 * - Source locations: attach line/column info for diagnostics
 *
 * @section usage Typical Usage
 * @code{.cpp}
 *   Module m;
 *   IRBuilder builder(m);
 *   auto &fn = builder.startFunction("main", Type(Type::Kind::Void), {});
 *   auto &entry = builder.createBlock(fn, "entry");
 *   builder.setInsertPoint(entry);
 *   builder.emitRet(std::nullopt, {});
 * @endcode
 *
 * @section design Design Philosophy
 * - Stateful: maintains insertion point for sequential code generation
 * - Fluent: methods return values that can be immediately used as operands
 * - Safe: validates block termination and SSA invariants
 * - Minimal: focused on IR construction (not analysis or transformation)
 *
 * @section lifetime Ownership/Lifetime
 * IRBuilder does not own the Module it operates on. The caller must ensure the
 * Module outlives all builder operations. Multiple builders may operate on the
 * same Module (but not concurrently on the same Function).
 */
//===----------------------------------------------------------------------===//

#pragma once

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Module.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/Value.hpp"
#include "support/source_location.hpp"
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace il::build {

/// @brief Helper to construct IL modules and enforce block termination.
class IRBuilder {
  public:
    /// @brief Create builder operating on module @p m.
    /// @param m Module to mutate.
    explicit IRBuilder(il::core::Module &m);

    /// @brief Add an external function declaration.
    /// @param name Symbol name.
    /// @param ret Return type.
    /// @param params Parameter types.
    /// @return Reference to newly added extern.
    il::core::Extern &addExtern(const std::string &name,
                                il::core::Type ret,
                                const std::vector<il::core::Type> &params);

    /// @brief Add a global variable.
    /// @param name Global identifier.
    /// @param type Variable type.
    /// @param init Optional initializer (empty for zero-initialized).
    /// @return Reference to newly added global.
    il::core::Global &addGlobal(const std::string &name,
                                il::core::Type type,
                                const std::string &init = "");

    /// @brief Add a global string constant.
    /// @param name Global identifier.
    /// @param value UTF-8 string literal.
    /// @return Reference to newly added global.
    il::core::Global &addGlobalStr(const std::string &name, const std::string &value);

    /// @brief Begin definition of function @p name.
    /// @param name Function identifier.
    /// @param ret Return type.
    /// @param params Parameter list.
    /// @return Reference to created function.
    il::core::Function &startFunction(const std::string &name,
                                      il::core::Type ret,
                                      const std::vector<il::core::Param> &params);

    /// @brief Create a basic block with optional parameters.
    /// @param fn Function receiving the block.
    /// @param label Block label.
    /// @param params Block parameter list.
    /// @return Reference to new block.
    il::core::BasicBlock &createBlock(il::core::Function &fn,
                                      const std::string &label,
                                      const std::vector<il::core::Param> &params = {});

    /// @brief Backward-compatible helper without parameters.
    /// @param fn Function receiving the block.
    /// @param label Unique block label.
    /// @return Reference to the appended parameter-less block.
    il::core::BasicBlock &addBlock(il::core::Function &fn, const std::string &label);

    /// @brief Insert a basic block at a specific index in @p fn.
    /// @param fn Function receiving the block.
    /// @param idx Zero-based insertion index into @ref il::core::Function::blocks.
    /// @param label Block label.
    /// @return Reference to the inserted block.
    /// @note This variant creates a parameter-less block and does not change the
    ///       current insertion point. Use when block order must precede an
    ///       existing block such as a synthetic exit block.
    il::core::BasicBlock &insertBlock(il::core::Function &fn, size_t idx, const std::string &label);

    /// @brief Access parameter @p idx of block @p bb as a value.
    /// @param bb Block whose parameter is referenced.
    /// @param idx Zero-based parameter index.
    /// @return Temporary value carrying the parameter's assigned SSA ID.
    /// @throws std::out_of_range when @p idx is not present.
    il::core::Value blockParam(il::core::BasicBlock &bb, unsigned idx);

    /// @brief Emit unconditional branch to @p dst with arguments @p args.
    /// @param dst Destination block.
    /// @param args Values bound to destination block parameters.
    /// @throws std::invalid_argument when argument arity does not match.
    void br(il::core::BasicBlock &dst, const std::vector<il::core::Value> &args = {});

    /// @brief Emit conditional branch.
    /// @param cond Condition value selecting the true or false edge.
    /// @param t True-edge destination.
    /// @param targs Values bound to @p t parameters.
    /// @param f False-edge destination.
    /// @param fargs Values bound to @p f parameters.
    /// @throws std::invalid_argument when either edge has incorrect arity.
    void cbr(const il::core::Value &cond,
             il::core::BasicBlock &t,
             const std::vector<il::core::Value> &targs,
             il::core::BasicBlock &f,
             const std::vector<il::core::Value> &fargs);

    /// @brief Set current insertion point to block @p bb.
    /// @param bb Target block.
    void setInsertPoint(il::core::BasicBlock &bb);

    /// @brief Emit reference to global string @p globalName.
    /// @param globalName Name of global string.
    /// @param loc Source location attached to the emitted instruction.
    /// @return Value representing constant string pointer.
    il::core::Value emitConstStr(const std::string &globalName, il::support::SourceLoc loc);

    /// @brief Emit call to function @p callee with arguments @p args.
    /// @param callee Name of function to call.
    /// @param args Argument values.
    /// @param dst Optional destination value to store result.
    /// @param loc Source location for diagnostics.
    /// @pre @p callee must have been previously registered via registerCallee().
    /// @throws std::logic_error if @p callee is unknown (programming error).
    void emitCall(const std::string &callee,
                  const std::vector<il::core::Value> &args,
                  const std::optional<il::core::Value> &dst,
                  il::support::SourceLoc loc);

    /// @brief Emit return from current function.
    /// @param v Optional return value.
    /// @param loc Source location attached to the return instruction.
    void emitRet(const std::optional<il::core::Value> &v, il::support::SourceLoc loc);

    /// @brief Emit resume that rethrows the current error within the same handler.
    /// @param token Resume token supplied by the active handler.
    /// @param loc Source location for diagnostics.
    void emitResumeSame(const il::core::Value &token, il::support::SourceLoc loc);

    /// @brief Emit resume that propagates to the next enclosing handler.
    /// @param token Resume token supplied by the active handler.
    /// @param loc Source location for diagnostics.
    void emitResumeNext(const il::core::Value &token, il::support::SourceLoc loc);

    /// @brief Emit resume to a specific handler block label.
    /// @param token Resume token supplied by the active handler.
    /// @param target Handler block receiving control.
    /// @param loc Source location for diagnostics.
    void emitResumeLabel(const il::core::Value &token,
                         il::core::BasicBlock &target,
                         il::support::SourceLoc loc);

    /// @brief Reserve the next SSA temporary identifier for the active function.
    /// @return Newly assigned temporary id.
    unsigned reserveTempId();

    /// @brief Assign a source-level display name to value @p id in the active
    ///        function, used by the debugger to surface named locals.
    /// @details No-op when there is no active function or @p name is empty. The
    ///          name is stored verbatim (no `%` prefix); the serializer prints it
    ///          as the value's identifier and `collectFrameLocals` treats any
    ///          non-`%` name as a source variable. Caller ensures uniqueness.
    /// @param id   Value/SSA id to name (typically an alloca slot result).
    /// @param name Source variable name to associate with @p id.
    void setValueName(unsigned id, const std::string &name);

    /// @brief Save the current temp ID counter (for lambda context switching).
    /// @return First currently unallocated SSA temporary identifier.
    unsigned saveTempId() const {
        return nextTemp;
    }

    /// @brief Restore a previously saved temp ID counter.
    /// @param saved Previously saved first-available temporary identifier.
    /// @throws std::logic_error if live IR in the active function already uses a
    ///         temporary at or above @p saved.
    void restoreTempId(unsigned saved);

    /// @brief Restore the current function pointer (for lambda context switching).
    /// @param fn Function to make active, or nullptr to clear the active function.
    /// @note This low-level context helper does not alter the insertion-block
    ///       index or temporary counter; callers restore those separately.
    void restoreFunction(il::core::Function *fn) {
        curFunc = fn;
    }

  private:
    il::core::Module &mod;                ///< Module being constructed
    il::core::Function *curFunc{nullptr}; ///< Current function
    std::optional<size_t> curBlockIdx{};  ///< Current insertion block index
    unsigned nextTemp{0};                 ///< Next temporary id
    std::unordered_map<std::string, il::core::Type>
        calleeReturnTypes; ///< Cached return types keyed by callee name

#ifndef NDEBUG
    /// @brief Hash sets for O(1) uniqueness checking in debug builds.
    /// @details These replace O(n) linear scans for name uniqueness validation.
    std::unordered_set<std::string> usedFunctionNames_; ///< Module-wide function names
    std::unordered_set<std::string> usedExternNames_;   ///< Module-wide extern names
    /// @brief Per-function block label tracking, keyed by function name.
    std::unordered_map<std::string, std::unordered_set<std::string>> usedBlockLabelsPerFunc_;
#endif

    /// @brief Append instruction @p instr to current block.
    /// @param instr Instruction to append.
    /// @return Reference to inserted instruction.
    il::core::Instr &append(il::core::Instr instr);

    /// @brief Check if opcode @p op terminates a block.
    /// @param op Opcode to test.
    /// @return True if op is a terminator.
    bool isTerminator(il::core::Opcode op) const;
};

} // namespace il::build
