//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/zia/DebugLayoutExport.hpp
// Purpose: Frontend-owned export shape for per-class field layouts consumed by
//          the VM debugger's class-instance expansion (ADR 0138).
// Key invariants:
//   * Pure data with no IL/VM dependencies; the tool layer performs conversion
//     to the VM's DebugClassLayoutTable.
//   * Field offsets are byte offsets from the object base pointer.
//   * Table keys are runtime class IDs stamped into allocated instances.
// Ownership: Value-owned by CompilerResult and captured after lowering.
// References: src/frontends/zia/Compiler.hpp, src/frontends/zia/Lowerer.hpp,
//        include/zanna/vm/debug/DebugClassLayout.hpp,
//        docs/adr/0138-debug-class-layout-sidecar.md
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares the frontend-owned debugger class-layout export shape.
/// @details Keeps debugger metadata transport independent of VM headers.
//
//===----------------------------------------------------------------------===//
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace il::frontends::zia {

/// @brief How the debugger should read a class field's storage (ADR 0138).
/// @details Frontend-owned mirror of the VM's DebugFieldStorage; values
///          describe the raw read at (object base + offset).
enum class DebugFieldStore : uint8_t {
    I64,     ///< 8-byte signed integer.
    I32,     ///< 4-byte signed integer (Byte fields).
    I16,     ///< 2-byte signed integer.
    I1,      ///< 1-byte boolean.
    F64,     ///< 8-byte IEEE double.
    Str,     ///< 8-byte runtime string handle.
    Managed, ///< 8-byte managed pointer (object/collection/box).
    Weak,    ///< 8-byte weak-reference slot (resolve via rt_weak_load).
    Raw,     ///< 8-byte raw pointer; opaque leaf.
    Opaque,  ///< Inline aggregate or unknown storage; typed leaf.
};

/// @brief One exported instance field of a user class (ADR 0138).
struct DebugFieldExport {
    std::string name;     ///< Source-level field name.
    std::string typeName; ///< Semantic type label for display.
    uint64_t offset = 0;  ///< Byte offset from the object base pointer.
    DebugFieldStore store = DebugFieldStore::Opaque; ///< Read strategy.
    bool boolDisplay = false; ///< Format integer storage as true/false.
};

/// @brief Exported field layout of one user class (inherited fields first).
struct DebugClassExport {
    std::string qname;                    ///< Fully-qualified class name.
    std::vector<DebugFieldExport> fields; ///< Instance fields in layout order.
};

/// @brief Runtime class id -> exported layout for the compiled module.
using DebugClassLayoutExport = std::unordered_map<int64_t, DebugClassExport>;

} // namespace il::frontends::zia
