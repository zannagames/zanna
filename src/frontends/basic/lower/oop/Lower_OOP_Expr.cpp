//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lower/oop/Lower_OOP_Expr.cpp
// Purpose: Stub file preserved for build compatibility.
//
// NOTE: The OOP expression lowering code has been split into focused files:
//   - Lower_OOP_Helpers.cpp   : resolveObjectClass and shared resolution helpers
//   - Lower_OOP_Alloc.cpp     : lowerNewExpr (object creation)
//   - Lower_OOP_MemberAccess.cpp : lowerMeExpr, resolveMemberField,
//                                  resolveImplicitField, lowerMemberAccessExpr
//   - Lower_OOP_MethodCall.cpp   : lowerMethodCallExpr
//
// This file is kept as a placeholder to avoid CMake source list churn.
// Future changes should go to the appropriate focused file above.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Preserves the historical OOP expression translation-unit boundary.
/// @details Implementations are intentionally split across allocation, member
///          access, method call, and shared helper files. This source owns no
///          symbols and remains in build manifests to avoid source-list churn.

// Intentionally empty - all implementations have been moved to focused files.
