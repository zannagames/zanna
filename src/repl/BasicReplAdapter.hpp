//===----------------------------------------------------------------------===//
/// @file
/// @brief Declares the BASIC implementation of the language-neutral REPL adapter.
/// @details The adapter owns procedure source, persistent-variable metadata,
///          and chronological replay statements. Each submitted fragment is
///          compiled in a fresh pipeline assembled from that stored session
///          state, and failed evaluations leave the accumulated state intact.
///
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/repl/BasicReplAdapter.hpp
// Purpose: BASIC-specific REPL adapter implementing the ReplAdapter interface.
//          Manages compilation of REPL inputs, session state tracking
//          (variables, subroutines), and BytecodeVM execution.
// Key invariants:
//   - Each input compiles to a fresh IL Module.
//   - Session variables persist across inputs via DIM replay.
//   - Failed compilations do not modify session state.
//   - Uses classifyBasic() for multi-line block keyword tracking.
// Ownership/Lifetime:
//   - Owns accumulated source fragments and the SourceManager.
//   - BytecodeVM is created per-eval.
// Links: src/repl/ReplSession.hpp, src/frontends/basic/BasicCompiler.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "ReplSession.hpp"

#include <map>
#include <string>
#include <vector>

namespace zanna::repl {

/// @brief Tracked persistent BASIC variable for cross-input replay.
/// @details The declaration and most recent assignment are retained as source
///          fragments so a subsequent synthetic compilation can reconstruct
///          the variable's session state.
struct BasicPersistentVar {
    std::string name;       ///< Variable name.
    std::string type;       ///< BASIC type (e.g., "Integer", "String").
    std::string dimStmt;    ///< Full DIM statement (e.g., "DIM x AS Integer = 42").
    std::string lastAssign; ///< Latest reassignment (e.g., "x = 100"), or empty.
};

/// @brief BASIC language REPL adapter.
/// @details Compiles each REPL input by building a synthetic BASIC source file
///          from accumulated state (DIM declarations, SUB/FUNCTION definitions)
///          plus the current input as top-level code.
class BasicReplAdapter : public ReplAdapter {
  public:
    /// @brief Construct an adapter with empty session state.
    BasicReplAdapter();
    /// @brief Destroy adapter-owned source fragments and metadata.
    ~BasicReplAdapter() override = default;

    /// @brief Compile and execute one BASIC submission transactionally.
    /// @param input Complete user submission.
    /// @return Evaluation result with captured output or diagnostic state.
    EvalResult eval(const std::string &input) override;
    /// @brief Produce case-insensitive keyword and session-name completions.
    /// @param input Current editor buffer.
    /// @param cursor Byte offset of the insertion point.
    /// @return Full-buffer completion alternatives.
    std::vector<std::string> complete(const std::string &input, size_t cursor) override;
    /// @brief Snapshot persistent variable names and types.
    /// @return Value-owned variable metadata in declaration order.
    std::vector<VarInfo> listVariables() const override;
    /// @brief Snapshot stored procedure names and display signatures.
    /// @return Value-owned procedure metadata.
    std::vector<FuncInfo> listFunctions() const override;
    /// @brief Report BASIC bind names.
    /// @return Empty collection because BASIC has no bind system.
    std::vector<std::string> listBinds() const override;
    /// @brief Identify the adapter's language.
    /// @return Static view containing `"basic"`.
    std::string_view languageName() const override;
    /// @brief Classify a fragment for single- or multi-line REPL handling.
    /// @param input BASIC source fragment.
    /// @return Classification from the shared BASIC input classifier.
    InputKind classifyInput(const std::string &input) override;
    /// @brief Discard all accumulated procedure and variable replay state.
    void reset() override;

  private:
    /// @brief Build the full synthetic BASIC source for compilation.
    /// @param input The current REPL input to include as top-level code.
    /// @return The complete BASIC program source.
    std::string buildSource(const std::string &input) const;

    /// @brief Try to compile source without execution (for expression type probing).
    /// @param source Complete synthetic BASIC source.
    /// @return True if compilation succeeded.
    bool tryCompileOnly(const std::string &source) const;

    /// @brief Compile and verify source only to produce diagnostics.
    /// @details Used when a validation probe fails so callers get either BASIC
    ///          frontend diagnostics or verifier diagnostics instead of a blank
    ///          error string.
    /// @param source Complete synthetic BASIC source to compile.
    /// @return Human-readable diagnostics, or an empty string if source is valid.
    std::string compileOnlyDiagnostic(const std::string &source) const;

    /// @brief Check if input looks like a SUB or FUNCTION definition.
    /// @param input BASIC source fragment.
    /// @return `true` when the leading keyword names a procedure definition.
    bool isSubOrFunc(const std::string &input) const;

    /// @brief Check if input looks like a variable declaration (DIM).
    /// @param input BASIC source fragment.
    /// @return `true` when the leading keyword is `DIM`.
    bool isDimDecl(const std::string &input) const;

    /// @brief Check if input is a bare assignment to a known variable.
    /// @param input BASIC source fragment.
    /// @return `true` for assignment to tracked persistent state.
    bool isAssignment(const std::string &input) const;

    /// @brief Check if input could be an expression (for auto-print).
    /// @param input BASIC source fragment.
    /// @return `true` when `PRINT`-wrapped compilation should be attempted.
    bool isLikelyExpression(const std::string &input) const;

    /// @brief Extract SUB/FUNCTION name from a definition.
    /// @param input BASIC procedure definition.
    /// @return Parsed name, or an empty string on mismatch.
    std::string extractProcName(const std::string &input) const;

    /// @brief Extract variable name and type from a DIM declaration.
    /// @param input BASIC declaration source.
    /// @return Name/type pair, using `Variant` when the type is omitted.
    std::pair<std::string, std::string> extractDimInfo(const std::string &input) const;

    /// @brief Extract assignment target name.
    /// @param input Known BASIC assignment source.
    /// @return Leading target identifier.
    std::string extractAssignTarget(const std::string &input) const;

    /// @brief Find a persistent variable by name.
    /// @param name Case-insensitive BASIC name.
    /// @return Mutable pointer into adapter-owned metadata, or `nullptr`.
    BasicPersistentVar *findPersistentVar(const std::string &name);
    /// @brief Find a persistent variable by name without mutation.
    /// @param name Case-insensitive BASIC name.
    /// @return Read-only pointer into adapter-owned metadata, or `nullptr`.
    const BasicPersistentVar *findPersistentVar(const std::string &name) const;

    /// @brief Compile and execute the given source, capturing stdout.
    /// @param source Complete synthetic BASIC program.
    /// @return Evaluation status, captured output, and diagnostic information.
    EvalResult compileAndRun(const std::string &source);

    // --- Session state ---
    std::map<std::string, std::string> definedProcs_; ///< name -> full source (SUB/FUNCTION)
    std::vector<BasicPersistentVar> persistentVars_;  ///< ordered persistent variables
    std::vector<std::string> replayStatements_;       ///< Chronological state replay statements
};

} // namespace zanna::repl
