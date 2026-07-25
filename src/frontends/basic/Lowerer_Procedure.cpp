//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file Lowerer_Procedure.cpp
/// @brief Stub for BASIC procedure lowering.
/// @details Procedure lowering was decomposed into focused modules to keep each
///          phase of the lowering pipeline isolated and well-documented:
///
/// - `Lowerer_Procedure_Context.cpp`
///   - LoweringContext construction and symbol table management helpers.
///   - Phase: context setup (runs before metadata collection).
/// - `Lowerer_Procedure_Signatures.cpp`
///   - Procedure signature collection and lookup.
///   - Phase: signature collection (runs during program scanning).
/// - `Lowerer_Procedure_Variables.cpp`
///   - Variable discovery, type inference, and storage resolution.
///   - Phase: variable collection (runs during metadata gathering).
/// - `Lowerer_Procedure_Skeleton.cpp`
///   - Block scheduling, skeleton construction, and slot allocation.
///   - Phase: block scheduling (runs after metadata collection, before emission).
/// - `Lowerer_Procedure_Emit.cpp`
///   - Procedure body emission, parameter materialization, and state reset.
///   - Phase: emission (final phase of procedure lowering).
//
//===----------------------------------------------------------------------===//

// This file intentionally left empty - all implementation moved to:
// Lowerer_Procedure_Context.cpp, Lowerer_Procedure_Signatures.cpp,
// Lowerer_Procedure_Variables.cpp, Lowerer_Procedure_Skeleton.cpp,
// Lowerer_Procedure_Emit.cpp
