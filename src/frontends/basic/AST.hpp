//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file serves as the umbrella header for the BASIC frontend Abstract Syntax
// Tree (AST) representation.
//
// The AST is the central data structure in the BASIC compilation pipeline:
//   Lexer → Parser → AST → Semantic → Lowerer → IL
//
// Purpose:
// This header aggregates all AST node type families into a single convenient
// include point, providing a complete view of the BASIC AST structure.
//
// AST Organization:
// The BASIC AST is organized into several node families:
// - DeclNodes: Declarations (Program, DimDecl, ProcedureDecl, FunctionDecl,
//   TypeDecl, NamespaceDecl, UsingDecl)
// - ExprNodes: Expressions (literals, identifiers, operators, function calls)
// - StmtNodes: Statements (assignments, control flow, I/O operations)
// - StmtBase/StmtControl/StmtDecl/StmtExpr: Statement base classes and variants
//
// Design Philosophy:
// - AST nodes support both read-only and mutable visitor dispatch so analysis
//   and rewriting passes can share one hierarchy
// - Nodes are allocated on the heap and managed via std::unique_ptr to enable
//   polymorphic storage and clear ownership semantics
// - Each node carries source location information for precise diagnostic reporting
// - The AST preserves source-level structure (statement ordering, nesting) before
//   semantic transformations
//
// Integration:
// - Produced by: Parser::parseProgram()
// - Consumed by: SemanticAnalyzer for validation and symbol resolution
// - Transformed by: Lowerer to generate IL instructions
// - Analyzed by: Various passes (type checking, control flow analysis)
//
// This umbrella header maintains backward compatibility for code that includes
// "AST.hpp" to access all node types.
//
//===----------------------------------------------------------------------===//

/**
 * @file AST.hpp
 * @brief Aggregates the complete public BASIC abstract-syntax-tree model.
 *
 * This compatibility umbrella supplies shared BASIC types, forward
 * declarations, declaration nodes, expression nodes, and every statement-node
 * family through one include. Concrete behavior and ownership contracts remain
 * documented in the family headers.
 */

#pragma once

/// Shared source ranges, tokens, and BASIC frontend value types.
#include "frontends/basic/BasicTypes.hpp"
/// Program and declaration node definitions.
#include "frontends/basic/ast/DeclNodes.hpp"
/// Expression base classes, visitors, and concrete expression nodes.
#include "frontends/basic/ast/ExprNodes.hpp"
/// Forward declarations for AST node and visitor types.
#include "frontends/basic/ast/NodeFwd.hpp"
/// Statement base classes, visitors, and every concrete statement family.
#include "frontends/basic/ast/StmtNodesAll.hpp"
