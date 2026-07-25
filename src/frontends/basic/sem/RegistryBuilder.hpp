//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/sem/RegistryBuilder.hpp
// Purpose: Declare the entry points that seed runtime semantic indexes and
//          collect namespace/type declarations from a BASIC program.
// Key invariants:
//   * Runtime catalog indexes are initialized before source declarations are
//     consumed by semantic analysis.
//   * File-scoped USING state is refreshed for each registry build.
// Ownership: Functions borrow the program and output registries; no references
//            to caller-owned inputs are retained.
// References: docs/internals/codemap/basic.md
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares BASIC semantic registry construction and runtime seeding.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/sem/NamespaceRegistry.hpp"
#include "frontends/basic/sem/UsingContext.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"

namespace il::frontends::basic {

// Forward declarations
struct Program;
class DiagnosticEmitter;

/// @brief Populate NamespaceRegistry and UsingContext from a parsed BASIC program.
/// @details Clears any pre-existing entries then walks the top-level statements
///          collecting namespace declarations, class declarations, interface declarations,
///          and USING directives. For each namespace the helper records the name; for
///          each class/interface it registers the type in the appropriate namespace.
/// @param program Parsed BASIC program supplying declarations.
/// @param registry Registry instance that receives namespace and type metadata.
/// @param usings Context that receives USING directives.
/// @param emitter Optional diagnostics interface for future checks.
void buildNamespaceRegistry(const Program &program,
                            NamespaceRegistry &registry,
                            UsingContext &usings,
                            DiagnosticEmitter *emitter);

/// @brief Seed runtime class-driven registries in one place.
/// @details Populates TypeRegistry, RuntimePropertyIndex, RuntimeMethodIndex,
///          and seeds NamespaceRegistry with class name prefixes.
/// @param registry Namespace registry that receives runtime class prefixes.
void seedRuntimeClassCatalogs(NamespaceRegistry &registry);

} // namespace il::frontends::basic
