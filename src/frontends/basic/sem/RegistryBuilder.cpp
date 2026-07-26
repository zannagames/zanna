//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/sem/RegistryBuilder.cpp
// Purpose: Populate BASIC namespace, USING, type, property, and method indexes
//          from runtime catalogs and parsed declarations.
// Key invariants:
//   * Runtime catalogs are seeded before user declarations are scanned.
//   * The namespace stack is restored after every nested namespace body.
//   * Only top-level USING directives enter the file-scoped UsingContext.
// Ownership: Output registries own copied metadata; the program AST and runtime
//            catalogs are borrowed for the duration of the build.
// References: docs/internals/codemap/basic.md
//
//===----------------------------------------------------------------------===//
///
/// @file
/// @brief Registry builder for namespace and type declarations in BASIC programs.
///
/// @details This file implements the `buildNamespaceRegistry` function which
/// populates the namespace registry by scanning a parsed BASIC program's AST.
/// It also initializes runtime type catalogs to enable type checking of
/// runtime library calls.
///
/// ## Overview
///
/// The registry builder performs several critical initialization tasks:
///
/// 1. **Runtime Namespace Seeding**: Seeds well-known namespaces (Zanna.*)
///    from the runtime library so that `USING Zanna.String` is valid.
///
/// 2. **Runtime Type Catalog Seeding**: Registers runtime classes, interfaces,
///    properties, and methods so they can be resolved during semantic analysis.
///
/// 3. **User Declaration Scanning**: Walks the AST to find user-defined
///    namespaces, classes, interfaces, and USING directives.
///
/// ## AST Walking Algorithm
///
/// The scanner uses a recursive lambda to walk the AST:
///
/// 1. Maintains a namespace stack (`nsStack`) for qualified name construction
/// 2. For each NAMESPACE declaration:
///    - Push path segments onto the stack
///    - Register the namespace in the registry
///    - Recurse into the namespace body
///    - Pop segments when exiting
/// 3. For CLASS and INTERFACE declarations:
///    - Register the type in the current namespace context
/// 4. For USING directives:
///    - Record file-scoped USINGs in the UsingContext
///    - Namespace-scoped USINGs are handled separately by SemanticAnalyzer
///
/// ## Qualified Name Construction
///
/// The `joinNs` helper constructs qualified names by joining namespace stack
/// segments with dots. For example, with stack ["Zanna", "Graphics"], it
/// produces "Zanna.Graphics".
///
/// ## Runtime Library Integration
///
/// Runtime types are seeded from multiple sources:
///
/// - **runtimeRegistry()**: Provides namespace information
/// - **runtimeClassCatalog()**: Provides class metadata (properties, methods)
/// - **RuntimeMethodIndex**: Provides method signature lookup (via RuntimeRegistry)
/// - **RuntimePropertyIndex**: Provides property type lookup
/// - **RuntimeTypeRegistry**: Provides type name registration
///
/// ## Example
///
/// For a BASIC program like:
/// ```basic
/// NAMESPACE MyApp
///     USING Zanna.String
///     CLASS Widget
///         ...
///     END CLASS
/// END NAMESPACE
/// ```
///
/// The builder will:
/// 1. Register namespace "MyApp"
/// 2. Skip the USING (it's namespace-scoped, handled by SemanticAnalyzer)
/// 3. Register class "Widget" in namespace "MyApp" as "MyApp.Widget"
///
/// @invariant The registry is cleared before each build operation.
/// @invariant Namespace stack is empty after processing completes.
/// @invariant Only file-scoped USING directives are added to UsingContext.
///
/// @see NamespaceRegistry - Target for namespace/type registration
/// @see UsingContext - Target for USING directive recording
/// @see RuntimeMethodIndex - Method signature lookup
/// @see il::runtime::runtimeClassCatalog - Source of runtime class metadata
///
//===----------------------------------------------------------------------===//

#include "frontends/basic/sem/RegistryBuilder.hpp"
#include "frontends/basic/AST.hpp"
#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/DiagnosticEmitter.hpp"
#include "frontends/basic/sem/RuntimeMethodIndex.hpp"
#include "frontends/basic/sem/RuntimePropertyIndex.hpp"
#include "frontends/basic/sem/TypeRegistry.hpp"
#include "il/runtime/RuntimeSignatures.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"

#include <functional>
#include <string>
#include <vector>

namespace il::frontends::basic {

/// @brief Populates the namespace registry and USING context from a BASIC program.
///
/// @details This function is the main entry point for registry initialization.
/// It performs three phases:
///
/// **Phase 1: Clear Previous State**
/// Clears the UsingContext to remove any stale data from previous compilations.
///
/// **Phase 2: Seed Runtime Catalogs**
/// Initializes all runtime type information:
/// - Seeds namespace prefixes from runtime builtins
/// - Seeds runtime type catalog (class names)
/// - Seeds property, method, and namespace indexes from runtime class catalog
///
/// **Phase 3: Scan User Declarations**
/// Walks the AST to find and register:
/// - Namespace declarations (NAMESPACE...END NAMESPACE)
/// - Class declarations (CLASS...END CLASS)
/// - Interface declarations (INTERFACE...END INTERFACE)
/// - File-scoped USING directives
///
/// ## Namespace Stack Algorithm
///
/// The function maintains a stack of namespace segments to construct qualified
/// names. When entering a NAMESPACE block, all path segments are pushed onto
/// the stack. When exiting, they are popped. This allows nested namespaces to
/// be handled correctly.
///
/// Example:
/// ```basic
/// NAMESPACE Zanna.Graphics
///     ' nsStack = ["Zanna", "Graphics"]
///     CLASS Canvas
///         ' Registered as "Zanna.Graphics.Canvas"
///     END CLASS
/// END NAMESPACE
/// ' nsStack = []
/// ```
///
/// @param program The parsed BASIC program containing declarations to process.
/// @param registry The namespace registry to populate with discovered namespaces
///                 and type names.
/// @param usings The USING context to populate with file-scoped USING directives.
/// @param emitter Optional diagnostic emitter for future validation warnings
///                (currently unused but reserved for extension).
///
/// @pre program must be a valid parsed Program structure.
/// @post registry contains all discovered namespaces and types.
/// @post usings contains all file-scoped USING directives.
/// @post Namespace stack is empty (all NAMESPACE blocks properly closed).
///
void buildNamespaceRegistry(const Program &program,
                            NamespaceRegistry &registry,
                            UsingContext &usings,
                            DiagnosticEmitter *emitter) {
    // Clear previous state to ensure fresh registration
    usings.clear();

    //==========================================================================
    // Phase 2: Seed runtime catalogs
    //==========================================================================

    // Seed runtime namespaces from built-in descriptors.
    // This enables validation of USING directives like "USING Zanna.String"
    registry.seedFromRuntimeBuiltins(il::runtime::runtimeRegistry());

    // Seed well-known runtime types (classes/interfaces) into the catalog.
    // This is catalog-only: registers namespaces and type names.
    // Member details (methods, properties) come from seedRuntimeClassCatalogs.
    seedRuntimeTypeCatalog(registry);

    // Seed class-driven registries from the RuntimeClasses catalog.
    // This populates:
    // - Type registry (class names)
    // - Property index (property types and getters/setters)
    // - Method index (via RuntimeRegistry delegation)
    // - Namespace prefixes (extracted from class qualified names)
    seedRuntimeClassCatalogs(registry);

    //==========================================================================
    // Phase 3: Scan user declarations
    //==========================================================================

    // Namespace stack for qualified name construction.
    // Segments are pushed when entering NAMESPACE blocks and popped when exiting.
    std::vector<std::string> nsStack;

    /// @brief Joins active namespace segments with dots.
    /// @return Qualified namespace, or an empty string at global scope.
    auto joinNs = [&]() -> std::string {
        if (nsStack.empty())
            return {};

        // Pre-calculate total length to avoid reallocations
        std::string result;
        std::size_t size = 0;
        for (const auto &s : nsStack)
            size += s.size() + 1;
        if (size)
            size -= 1; // No trailing dot needed

        result.reserve(size);
        for (std::size_t i = 0; i < nsStack.size(); ++i) {
            if (i)
                result.push_back('.');
            result += nsStack[i];
        }
        return result;
    };

    // Recursive lambda to scan statements and populate the registry.
    // This is the core AST walker that finds declarations.
    std::function<void(const std::vector<StmtPtr> &)> scan;
    /// @brief Recursively scans declarations into the semantic registry.
    /// @param stmts Statement list in the active namespace.
    scan = [&](const std::vector<StmtPtr> &stmts) {
        for (const auto &stmtPtr : stmts) {
            if (!stmtPtr)
                continue;

            switch (stmtPtr->stmtKind()) {
                //--------------------------------------------------------------
                // NAMESPACE declaration
                //--------------------------------------------------------------
                case Stmt::Kind::NamespaceDecl: {
                    const auto &ns = static_cast<const NamespaceDecl &>(*stmtPtr);

                    // Push all path segments onto the namespace stack.
                    // NAMESPACE Foo.Bar pushes ["Foo", "Bar"]
                    for (const auto &seg : ns.path)
                        nsStack.push_back(seg);

                    // Register the namespace in the registry
                    std::string nsFull = joinNs();
                    if (!nsFull.empty())
                        registry.registerNamespace(nsFull);

                    // Recurse into the namespace body
                    scan(ns.body);

                    // Pop segments when exiting the namespace
                    nsStack.resize(
                        nsStack.size() >= ns.path.size() ? nsStack.size() - ns.path.size() : 0);
                    break;
                }

                //--------------------------------------------------------------
                // CLASS declaration
                //--------------------------------------------------------------
                case Stmt::Kind::ClassDecl: {
                    const auto &classDecl = static_cast<const ClassDecl &>(*stmtPtr);
                    std::string nsFull = joinNs();

                    // Register the class in the current namespace context.
                    // If nsFull is empty, this is a top-level class.
                    registry.registerClass(nsFull, classDecl.name);
                    break;
                }

                //--------------------------------------------------------------
                // INTERFACE declaration
                //--------------------------------------------------------------
                case Stmt::Kind::InterfaceDecl: {
                    const auto &ifaceDecl = static_cast<const InterfaceDecl &>(*stmtPtr);

                    // InterfaceDecl.qualifiedName contains the full path including
                    // the type name. Extract namespace (all but last segment) and
                    // name (last segment).
                    if (!ifaceDecl.qualifiedName.empty()) {
                        std::string ifaceName = ifaceDecl.qualifiedName.back();
                        std::string nsFull;

                        if (ifaceDecl.qualifiedName.size() > 1) {
                            // Build namespace from all segments except the last
                            for (std::size_t i = 0; i + 1 < ifaceDecl.qualifiedName.size(); ++i) {
                                if (i > 0)
                                    nsFull.push_back('.');
                                nsFull += ifaceDecl.qualifiedName[i];
                            }
                        }

                        // Register the interface in its namespace
                        registry.registerInterface(nsFull, ifaceName);
                    }
                    break;
                }

                //--------------------------------------------------------------
                // USING directive
                //--------------------------------------------------------------
                case Stmt::Kind::UsingDecl: {
                    const auto &usingDecl = static_cast<const UsingDecl &>(*stmtPtr);

                    // Build dotted namespace path from segments
                    std::string nsPath;
                    for (std::size_t i = 0; i < usingDecl.namespacePath.size(); ++i) {
                        if (i)
                            nsPath.push_back('.');
                        nsPath += usingDecl.namespacePath[i];
                    }

                    // Only record file-scoped USING directives in UsingContext.
                    // USING directives inside NAMESPACE blocks are handled by
                    // SemanticAnalyzer's scoped usingStack_ and must not leak
                    // into file-scoped context.
                    if (nsStack.empty() && !nsPath.empty())
                        usings.add(std::move(nsPath), usingDecl.alias, usingDecl.loc);
                    break;
                }

                default:
                    // Other statement types are not relevant for registry building
                    break;
            }
        }
    };

    // Start scanning from the program's main statement list
    scan(program.main);
}

/// @brief Seeds all runtime class-driven catalogs from the RuntimeClasses system.
///
/// @details This function initializes the various runtime indexes from the
/// centralized RuntimeClasses catalog. It ensures that the BASIC frontend has
/// access to all runtime type information for semantic analysis.
///
/// The seeding order matters:
/// 1. **Types first**: So type names can be resolved
/// 2. **Properties**: So property accesses can be type-checked
/// 3. **Methods**: So method calls can be validated (delegates to RuntimeRegistry)
/// 4. **Namespaces**: So namespace prefixes from class names are available
///
/// @param registry The namespace registry to seed with class namespace prefixes.
///
void seedRuntimeClassCatalogs(NamespaceRegistry &registry) {
    const auto &classes = il::runtime::runtimeClassCatalog();

    // Seed type registry with runtime class names
    runtimeTypeRegistry().seedRuntimeClasses(classes);

    // Seed property index with runtime class properties
    runtimePropertyIndex().seed(classes);

    // Seed method index - this is now a no-op since RuntimeMethodIndex
    // delegates to RuntimeRegistry which builds its own indexes
    runtimeMethodIndex().seed();

    // Seed namespace prefixes extracted from runtime class qualified names
    // (e.g., "Zanna.String" -> register "Zanna" namespace)
    registry.seedRuntimeClassNamespaces(classes);
}

} // namespace il::frontends::basic
