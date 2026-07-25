//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lower/oop/Lower_OOP_RuntimeHelpers.hpp
// Purpose: Consolidated OOP runtime emission helpers for BASIC lowering.
// Key invariants: Centralizes patterns for parameter initialization, array field
//                 allocation, and method epilogue. (BUG-056, BUG-073, BUG-089, etc.)
// Ownership/Lifetime: Non-owning references to Lowerer and OOP metadata.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/AST.hpp"
#include "frontends/basic/LowererTypes.hpp"
#include "frontends/basic/OopIndex.hpp"
#include "zanna/il/Module.hpp"

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

/// @file
/// @brief Declares reusable OOP emission operations shared by class member
///        lowering paths.

namespace il::frontends::basic {

class Lowerer;

/// @brief Consolidated helper for OOP runtime emission patterns.
/// @details Provides reusable implementations of patterns that were duplicated
///          across emitClassConstructor, emitClassMethod, emitClassMethodWithBody,
///          and interface binding thunks. Centralizing these patterns reduces
///          code duplication and ensures consistent handling of BUG fixes.
class OopEmitHelper {
  public:
    /// @brief Construct a helper bound to the lowering context.
    /// @param lowerer Owning lowering driver.
    explicit OopEmitHelper(Lowerer &lowerer) noexcept;

    // -------------------------------------------------------------------------
    // Parameter Initialization
    // -------------------------------------------------------------------------

    /// @brief Initialize a single object or array parameter.
    /// @details Allocates a slot, sets object type if applicable,
    ///          marks the symbol as referenced, and stores the incoming value.
    /// @param param Parameter AST node describing name, type, and flags.
    /// @param fn Function being lowered.
    /// @param paramIdx Zero-based index into fn.params (after ME if present).
    /// @param metadata ProcedureMetadata to update with param name.
    void emitParamInit(const Param &param,
                       il::core::Function &fn,
                       std::size_t paramIdx,
                       std::unordered_set<std::string> &paramNames);

    /// @brief Initialize all parameters for a method or constructor.
    /// @details Calls emitParamInit for each parameter in sequence.
    /// @param params Parameter list from the AST.
    /// @param fn Function being lowered.
    /// @param selfOffset 1 for instance methods (ME parameter), 0 for static.
    /// @param paramNames Set to populate with parameter names.
    void emitAllParamInits(const std::vector<Param> &params,
                           il::core::Function &fn,
                           std::size_t selfOffset,
                           std::unordered_set<std::string> &paramNames);

    // -------------------------------------------------------------------------
    // Array Field Initialization
    // -------------------------------------------------------------------------

    /// @brief Initialize array fields declared with extents in a constructor.
    /// @details For each array field with declared dimensions, allocates an
    ///          appropriate array handle (i32, str, or obj) and stores it into
    ///          the instance field slot.
    /// @param klass Class declaration containing field definitions.
    /// @param selfSlotId Stack slot ID holding the ME pointer.
    void emitArrayFieldInits(const ClassDecl &klass, unsigned selfSlotId);

    // -------------------------------------------------------------------------
    // Method Epilogue
    // -------------------------------------------------------------------------

    /// @brief Emit the standard method/constructor epilogue.
    /// @details Releases deferred temporaries, object locals, and array locals.
    ///          Borrowed parameters are not released (passed by reference). (BUG-105)
    /// @param paramNames Set of parameter names to exclude from local release.
    /// @param excludeFromObjRelease Additional names to exclude (e.g., method name for object
    /// returns).
    void emitMethodEpilogue(const std::unordered_set<std::string> &paramNames,
                            const std::unordered_set<std::string> &excludeFromObjRelease);

    // -------------------------------------------------------------------------
    // Body Statement Lowering
    // -------------------------------------------------------------------------

    /// @brief Lower body statements and branch to exit if not terminated.
    /// @details Calls lowerStatementSequence and emits branch to exit block
    ///          if the current block is not already terminated.
    /// @param bodyStmts Statements to lower.
    /// @param exitIdx Index of the exit block in the function.
    void emitBodyAndBranchToExit(const std::vector<const Stmt *> &bodyStmts, std::size_t exitIdx);

    // -------------------------------------------------------------------------
    // VTable/ITable Population (duplicated logic consolidated)
    // -------------------------------------------------------------------------

    /// @brief Find the concrete implementor class for a method along the base chain.
    /// @details Walks up the inheritance hierarchy to find the most derived
    ///          class that provides a non-abstract implementation of the method.
    /// @param oopIndex OOP metadata index.
    /// @param startQClass Qualified class name to start searching from.
    /// @param methodName Method name to look up.
    /// @return Qualified class name of the implementor, or startQClass if not found.
    static std::string findImplementorClass(const OopIndex &oopIndex,
                                            const std::string &startQClass,
                                            const std::string &methodName);

    /// @brief Build a slot-to-method-name mapping for vtable population.
    /// @details Walks the class hierarchy to collect all virtual method slots
    ///          and their corresponding method names.
    /// @param oopIndex OOP metadata index.
    /// @param classQName Qualified class name.
    /// @param maxSlot Output parameter for the maximum slot index found.
    /// @return Vector mapping slot index to method name (empty string for unused slots).
    static std::vector<std::string> buildVtableSlotMap(const OopIndex &oopIndex,
                                                       const std::string &classQName,
                                                       std::size_t &maxSlot);

  private:
    /// Borrowed lowering driver receiving symbol and IL mutations.
    Lowerer &lowerer_;
};

} // namespace il::frontends::basic
