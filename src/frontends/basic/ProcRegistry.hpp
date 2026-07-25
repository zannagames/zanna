//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares the ProcRegistry class, which manages BASIC procedure
// (SUB and FUNCTION) signatures and validates procedure declarations and calls.
//
// Procedure Management:
// The ProcRegistry tracks all user-defined procedures in a BASIC program,
// maintaining their signatures for:
// - Forward reference validation: Ensuring calls to procedures declared later
//   in the program are valid
// - Signature checking: Verifying that procedure calls match the declared
//   parameter count and types
// - Duplicate detection: Reporting errors when procedures are defined multiple
//   times with conflicting signatures
//
// Two-Pass Processing:
// The registry supports the semantic analyzer's two-pass approach:
// 1. Declaration pass: Collects all SUB and FUNCTION signatures from the AST
// 2. Validation pass: Checks that all calls match registered signatures
//
// Procedure Signature Information:
// For each procedure, the registry stores:
// - Name: Procedure identifier (case-insensitive in BASIC)
// - Parameters: List of parameter types (Integer, Long, Single, Double, String)
// - Return type: For FUNCTION declarations, the return type; SUB is empty
// - Declaration location: Source location for error reporting
//
// Call Validation:
// Semantic analysis uses registry signatures to check:
// - The procedure name is defined
// - The argument count matches the parameter count
// - Argument types are compatible with parameter types
// - Functions are called in expression context
// - SUBs are called as statements
//
// Integration:
// - Used by: SemanticAnalyzer during both passes
// - Borrows: SemanticDiagnostics for error reporting
// - No AST ownership: The registry only stores signature metadata
//
// Design Notes:
// - Canonical procedure keys provide case-insensitive lookup
// - Each procedure name maps to exactly one signature; redefinitions are errors
// - The registry copies signature/location metadata and does not own AST nodes
//
//===----------------------------------------------------------------------===//
/// @file ProcRegistry.hpp
/// @brief Declares BASIC procedure signature registration and lookup.
/// @details ProcRegistry owns copied signature and entry metadata, borrows only
///          SemanticDiagnostics, and pre-seeds supported dotted runtime
///          functions. User declarations are canonicalized for case-insensitive
///          lookup and checked against existing user and builtin names.

#pragma once

#include "il/runtime/RuntimeSignatures.hpp"
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "frontends/basic/SemanticDiagnostics.hpp"
#include "frontends/basic/ast/DeclNodes.hpp"

namespace il::frontends::basic {

/// @brief Hash functor for heterogeneous string lookup (C++20).
struct ProcStringHash {
    /// Marker enabling heterogeneous lookup in compatible unordered containers.
    using is_transparent = void;

    /// @brief Hash a string-like key through its `std::string_view` representation.
    /// @tparam T Type constructible as `std::string_view`.
    /// @param key String-like value to hash.
    /// @return Standard-library hash of the character sequence.
    template <typename T> [[nodiscard]] std::size_t operator()(const T &key) const noexcept {
        return std::hash<std::string_view>{}(std::string_view(key));
    }
};

/// @brief Semantic call signature stored for a BASIC or runtime procedure.
struct ProcSignature {
    /// @brief Whether the callable produces a value.
    enum class Kind { Function, Sub } kind{Kind::Function};
    /// BASIC return type for functions; empty for subroutines.
    std::optional<Type> retType;

    /// @brief One parameter's BASIC type and array classification.
    struct Param {
        /// Scalar or element type.
        Type type{Type::I64};
        /// Whether the declaration passes an array.
        bool is_array{false};
    };

    /// Parameters in declaration order.
    std::vector<Param> params;

    /// @brief True when this signature was imported from the runtime catalog.
    bool isRuntimeBuiltin{false};

    /// @brief True when a runtime builtin returns an object reference.
    /// @details The AST @ref Type enum cannot express object returns, so runtime
    ///          helpers returning `obj` are seeded with @ref retType `I64`. This
    ///          flag preserves the object-ness so semantic analysis can type the
    ///          call result as OBJECT instead of INTEGER.
    bool objectReturn{false};

    /// @brief Canonical runtime target name for imported helpers.
    std::string runtimeTarget;

    /// @brief Whether the runtime helper returns an unsafe raw pointer.
    bool rawPointerReturn{false};

    /// @brief Per-parameter unsafe raw pointer flags from the runtime catalog.
    std::vector<bool> rawPointerParams;

    /// @brief Per-parameter object flags for runtime builtins.
    /// @details Object parameters are seeded with @ref Param::type `I64` because
    ///          the AST type enum cannot express objects; this mask preserves the
    ///          object-ness so argument checking can reject primitives.
    std::vector<bool> objectParams;
};

/// Signature map supporting heterogeneous string lookup.
using ProcTable = std::unordered_map<std::string, ProcSignature, ProcStringHash, std::equal_to<>>;

/// @brief Owns procedure signatures and qualified conflict metadata.
/// @details Runtime builtins are inserted at construction and after @ref clear.
///          The registry retains no AST pointers for normal registration;
///          diagnostics are emitted through the borrowed service.
class ProcRegistry {
  public:
    /// @brief Construct a registry and seed supported runtime procedures.
    /// @param d Diagnostic service borrowed for this object's lifetime.
    /// @pre @p d outlives the registry.
    explicit ProcRegistry(SemanticDiagnostics &d);

    /// @brief Remove current entries and reseed supported runtime procedures.
    void clear();

    /// @brief Origin classification for a qualified registry entry.
    enum class ProcKind : std::uint8_t { User, BuiltinExtern };

    /// @brief Conflict and runtime back-pointer metadata for one canonical name.
    struct ProcEntry {
        /// Reserved non-owning declaration pointer; currently null for inserted entries.
        const void *node{nullptr};
        /// Declaration or registration location.
        il::support::SourceLoc loc{};
        /// Whether the entry is user-defined or runtime-seeded.
        ProcKind kind{ProcKind::User};
        /// Runtime signature ID for a builtin entry when one is available.
        std::optional<il::runtime::RtSig> runtimeSigId{};
    };

    /// @brief Register a user FUNCTION signature.
    /// @param f Declaration whose signature is copied; no AST pointer is retained.
    void registerProc(const FunctionDecl &f);

    /// @brief Register a user SUB signature.
    /// @param s Declaration whose signature is copied; no AST pointer is retained.
    void registerProc(const SubDecl &s);

    /// @brief Access all display and canonical signature entries.
    /// @return Const reference to registry-owned signature storage.
    const ProcTable &procs() const;

    /// @brief Look up a display spelling, then its canonicalized equivalent.
    /// @param name Qualified or unqualified procedure spelling.
    /// @return Pointer to a registry-owned signature, or null when absent/invalid.
    const ProcSignature *lookup(std::string_view name) const;

    /// @brief Register a FUNCTION through the legacy pointer-based API.
    /// @param fn Declaration to copy, or null for no operation.
    /// @param loc Registration location overriding the declaration location.
    void AddProc(const FunctionDecl *fn, il::support::SourceLoc loc);
    /// @brief Probe the qualified-entry table without canonicalization.
    /// @param qualified Exact table key.
    /// @return Pointer to registry-owned entry metadata, or null when absent.
    const ProcEntry *LookupExact(std::string_view qualified) const;

    /// @brief Add supported dotted runtime signatures not already registered.
    void seedRuntimeBuiltins();

  private:
    /// @brief Borrowed view of declaration data used by shared registration.
    struct ProcDescriptor {
        /// Function or subroutine classification.
        ProcSignature::Kind kind{ProcSignature::Kind::Function};
        /// Optional BASIC return type.
        std::optional<Type> retType;
        /// Borrowed parameter sequence valid for the registration call.
        std::span<const Param> params;
        /// Source location carried with the descriptor.
        il::support::SourceLoc loc;
    };

    /// @brief Validate parameters and copy a descriptor into a stored signature.
    /// @param descriptor Borrowed procedure description.
    /// @return Owned signature value.
    ProcSignature buildSignature(const ProcDescriptor &descriptor);

    /// @brief Canonicalize, conflict-check, and store one procedure descriptor.
    /// @param name Display or dotted procedure spelling.
    /// @param descriptor Procedure data to validate and copy.
    /// @param loc Location used for diagnostics and qualified entry metadata.
    void registerProcImpl(std::string_view name,
                          const ProcDescriptor &descriptor,
                          il::support::SourceLoc loc);

    /// Borrowed semantic diagnostic service.
    SemanticDiagnostics &de;
    /// Owned signatures indexed by display and canonical spellings.
    ProcTable procs_;
    /// Owned conflict/runtime metadata indexed by canonical qualified key.
    std::unordered_map<std::string, ProcEntry, ProcStringHash, std::equal_to<>> byQualified_;
};

} // namespace il::frontends::basic

// RtSig is now available from included header.
