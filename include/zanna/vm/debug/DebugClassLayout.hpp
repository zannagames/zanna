//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: include/zanna/vm/debug/DebugClassLayout.hpp
// Purpose: Plain-data class-layout sidecar the host installs on the debug
//          controller so the VM can expand user class instances field-by-field
//          at a stop (ADR 0138).
// Key invariants:
//   - Pure data: no VM, runtime, or frontend dependencies. The host (tool)
//     converts the frontend's export into this shape; the VM only reads it.
//   - Field offsets are byte offsets from the object base pointer (the value
//     rt_obj_new_i64 returned), exactly as the frontend's GEPs use them.
//   - Table keys are the compiler-assigned runtime class ids (positive for
//     user classes; builtin runtime ids are negative and never appear here).
// Ownership/Lifetime:
//   - Owned by DebugCtrl for the duration of a debug run; stop-time variable
//     stores borrow a const pointer that never outlives the run.
// Links: include/zanna/vm/debug/DebugFrontend.hpp, src/vm/debug/VMDebug.cpp,
//        docs/adr/0138-debug-class-layout-sidecar.md
//
//===----------------------------------------------------------------------===//

/**
 * @file include/zanna/vm/debug/DebugClassLayout.hpp
 * @brief Defines the dependency-light class-layout metadata consumed by the VM
 *        debugger.
 *
 * @details
 * A frontend or host converts its semantic class layout into these owning data
 * records before execution. At a debugger stop, the VM looks up the live
 * object's runtime class identifier and uses each field's byte offset and
 * storage strategy to create expandable variable data.
 *
 * This sidecar deliberately contains no frontend, runtime, or concrete VM
 * objects. Offsets and storage tags must already reflect the target ABI.
 */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace il::vm {

/**
 * @brief Selects how the debugger interprets bytes at an instance-field offset.
 *
 * The enumerator describes storage representation rather than source-language
 * semantics; `DebugFieldLayout::typeName` retains the semantic display label.
 */
enum class DebugFieldStorage : uint8_t {
    I64,     ///< 8-byte signed integer.
    I32,     ///< 4-byte signed integer (e.g. Byte fields).
    I16,     ///< 2-byte signed integer.
    I1,      ///< 1-byte boolean.
    F64,     ///< 8-byte IEEE double.
    Str,     ///< 8-byte runtime string handle (may be null).
    Managed, ///< 8-byte managed pointer (object/list/seq/map/box; may be null).
    Weak,    ///< 8-byte weak-reference slot; resolve via rt_weak_load.
    Raw,     ///< 8-byte raw pointer; shown as an opaque leaf, never dereferenced.
    Opaque,  ///< Inline aggregate or unknown storage; shown as a typed leaf.
};

/**
 * @brief Describes one debugger-visible instance field.
 *
 * @invariant `offset` is measured from the object payload address returned by
 *            the runtime allocator.
 * @invariant `storage` matches the byte width and ownership representation used
 *            by generated field accesses.
 */
struct DebugFieldLayout {
    std::string name;     ///< Source-level field name.
    std::string typeName; ///< Semantic type label (e.g. "Integer", "List[Str]").
    uint64_t offset = 0;  ///< Byte offset from the object base pointer.
    DebugFieldStorage storage = DebugFieldStorage::Opaque; ///< Read strategy.
    bool boolDisplay = false; ///< Format integer storage as true/false.
};

/**
 * @brief Owns the debugger-visible instance layout for one user class.
 *
 * Fields appear in physical layout order, with inherited fields preceding
 * fields introduced by the class.
 */
struct DebugClassLayout {
    std::string qname;                    ///< Fully-qualified class name.
    std::vector<DebugFieldLayout> fields; ///< Instance fields in layout order.
};

/**
 * @brief Maps compiler-assigned runtime class identifiers to owning layouts.
 *
 * Keys match `rt_obj_type_id` for live instances. Positive user-class IDs are
 * expected; negative built-in runtime type IDs are expanded through their
 * specialized debugger paths instead.
 */
using DebugClassLayoutTable = std::unordered_map<int64_t, DebugClassLayout>;

} // namespace il::vm
