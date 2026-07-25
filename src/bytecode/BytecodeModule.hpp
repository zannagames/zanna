//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/bytecode/BytecodeModule.hpp
// Purpose: Data structures for compiled bytecode modules and functions.
// Key invariants: Constant pool entries are deduplicated (same value -> same index).
//                 Function indices are stable after insertion.
//                 Module magic and version are set at construction time.
// Ownership: BytecodeModule owns all contained functions, pools, and metadata.
// Lifetime: Created by BytecodeCompiler; consumed by BytecodeVM. The module
//           must outlive any VM executing it.
// Links: Bytecode.hpp, BytecodeCompiler.hpp, BytecodeVM.hpp
//
//===----------------------------------------------------------------------===//
//
// This file defines the data structures that represent a compiled bytecode
// module ready for execution by the bytecode VM. BytecodeModule contains
// all the information needed to execute a program: functions, constant pools,
// native function references, and optional debug information.

/**
 * @file src/bytecode/BytecodeModule.hpp
 * @brief Defines the owning in-memory representation of compiled bytecode.
 *
 * @details
 * A module combines versioned header data, deduplicated constant pools,
 * compiled functions, native-call references, global descriptors, and source
 * metadata. Name-to-index maps accelerate lookup while vectors preserve the
 * stable numeric indices encoded by bytecode instructions.
 *
 * Returned pointers and references into module vectors are borrowed and may be
 * invalidated by later insertions. A module must outlive every VM executing it.
 */

#pragma once

#include "bytecode/Bytecode.hpp"
#include "il/core/Type.hpp"
#include "il/runtime/RuntimeSignatures.hpp"
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zanna {
namespace bytecode {

namespace detail {
/// @brief Find or add a value to a pool with deduplication.
/// @details Performs a linear scan for an existing match. If found, returns
///          its index; otherwise appends the value and returns the new index.
///          This ensures the same constant value always maps to the same pool index.
///          Throws when the pool would exceed the module's 32-bit index model.
/// @tparam T Element type stored in the pool.
/// @tparam Eq Equality predicate type for comparing elements.
/// @param pool  The pool vector to search and potentially append to.
/// @param value The value to find or add.
/// @param eq    Equality comparison function.
/// @return Index of the value in the pool (existing or newly appended).
/// @throws std::length_error When a new entry would exceed 32-bit indexing.
/// @throws std::bad_alloc If vector growth or element copying fails.
template <typename T, typename Eq>
inline uint32_t findOrAddToPool(std::vector<T> &pool, const T &value, Eq eq) {
    for (size_t i = 0; i < pool.size(); ++i) {
        if (eq(pool[i], value)) {
            return static_cast<uint32_t>(i);
        }
    }
    if (pool.size() > std::numeric_limits<uint32_t>::max())
        throw std::length_error("bytecode constant pool exceeds 32-bit index width");
    uint32_t idx = static_cast<uint32_t>(pool.size());
    pool.push_back(value);
    return idx;
}

/// @brief Build the deduplication key for a native function reference.
/// @details Native calls are encoded with both a name and call-site shape.
///          Using all signature-affecting fields avoids reusing a native-table
///          entry created by a different overload or malformed call site.
/// @param name Native/runtime function name.
/// @param paramCount Number of arguments encoded at the call site.
/// @param hasReturn Whether the call site expects a return value.
/// @return Stable key suitable for @ref BytecodeModule::nativeFuncIndex.
inline std::string nativeFunctionKey(const std::string &name, uint32_t paramCount, bool hasReturn) {
    return name + "#" + std::to_string(paramCount) + "#" + (hasReturn ? "ret" : "void");
}
} // namespace detail

/// @brief Debug information about a local variable within a bytecode function.
/// @details Maps a source-level variable name to its runtime local slot and
///          the PC range during which it is live (for debugger display).
struct LocalVarInfo {
    std::string name;  ///< Original source-level variable name.
    uint32_t localIdx = 0; ///< Index in the function's locals array.
    uint32_t startPc = 0;  ///< First PC where the variable is live (inclusive).
    uint32_t endPc = 0;    ///< Last PC where the variable is live (exclusive).
};

/// @brief An exception handler range within a bytecode function.
/// @details Defines a protected PC region and the handler entry point to
///          jump to when a trap occurs within that region.
struct ExceptionRange {
    uint32_t startPc;   ///< Range start PC (inclusive).
    uint32_t endPc;     ///< Range end PC (exclusive).
    uint32_t handlerPc; ///< Handler entry point PC.
};

/// @brief A single case entry in a switch table.
struct SwitchEntry {
    int64_t value;     ///< The case value to match against.
    uint32_t targetPc; ///< Target PC when this case matches.
};

/// @brief Switch table for the SWITCH opcode.
/// @details Contains a default target and a list of case entries. The VM
///          performs a linear scan or binary search over entries to find
///          a matching case value.
struct SwitchTable {
    uint32_t defaultPc = 0;           ///< Default target PC when no case matches.
    std::vector<SwitchEntry> entries; ///< Ordered list of case entries.
};

/// @brief A compiled bytecode function ready for execution.
/// @details Contains the bytecode instruction stream, local/stack sizing info,
///          exception handler ranges, switch tables, and optional debug metadata.
struct BytecodeFunction {
    std::string name;        ///< Fully qualified function name.
    uint32_t numParams = 0;  ///< Number of parameters (mapped to first N locals).
    uint32_t numLocals = 0;  ///< Total local slots (parameters + temporaries).
    uint32_t maxStack = 0;   ///< Maximum operand stack depth required during execution.
    uint32_t allocaSize = 0; ///< Maximum alloca bytes needed by the function.
    bool hasReturn = false;  ///< True if the function returns a value; false for void.

    std::vector<uint32_t> code;         ///< Bytecode instruction stream (32-bit words).
    std::vector<uint8_t> localIsString; ///< 1 when the corresponding local slot stores Str.

    /// @brief Exception handler ranges active in this function.
    std::vector<ExceptionRange> exceptionRanges;

    /// @brief Switch tables referenced by SWITCH instructions in this function.
    std::vector<SwitchTable> switchTables;

    // Debug info (optional)
    std::vector<LocalVarInfo> localVars; ///< Local variable debug information.
    uint32_t sourceFileIdx = 0;          ///< Default index into the module's source file list.
    std::vector<uint32_t> lineTable;     ///< PC-to-source-line mapping (indexed by PC).
    std::vector<uint32_t>
        sourceFileTable; ///< PC-to-source-file mapping (1-based index, 0 when unknown).
    std::vector<std::string> blockLabelTable; ///< PC-to-basic-block label mapping.
};

/// @brief Reference to a native/runtime function callable from bytecode.
/// @details Stores the name and signature information needed to locate and
///          invoke a native function through the RuntimeBridge or registered handlers.
struct NativeFuncRef {
    std::string name;        ///< Function name (e.g., "Zanna.Terminal.Say").
    uint32_t paramCount = 0; ///< Number of parameters the function expects.
    bool hasReturn = false;  ///< True if the function returns a value.
    const il::runtime::RuntimeDescriptor *runtimeDescriptor =
        nullptr;                                                     ///< Cached runtime descriptor.
    const il::runtime::RuntimeSignature *runtimeSignature = nullptr; ///< Cached runtime signature.
    bool consumesClonedStringArgs = false; ///< True when bridge needs retained string aliases.
    bool consumesOwnedStringArgs = false;  ///< True when runtime consumes original string args.
    bool returnsString = false;            ///< True when runtime result is a managed string.
};

/// @brief Information about a global variable in the bytecode module.
/// @details Globals are laid out as contiguous BCSlot entries. Each GlobalInfo
///          describes the name, size, alignment, and optional initial data.
struct GlobalInfo {
    std::string name; ///< Fully qualified global variable name.
    uint32_t size = 0;  ///< Size of the global in bytes.
    uint32_t align = 0; ///< Alignment requirement in bytes.
    il::core::Type type = il::core::Type(il::core::Type::Kind::I64); ///< IL storage type.
    std::vector<uint8_t> initData; ///< Initial data bytes (empty means zero-initialized).
    std::string initString;        ///< Initial string payload for Str globals.
};

/// @brief Source file reference for debug information.
struct SourceFileInfo {
    std::string path;  ///< File path of the source file.
    uint32_t checksum = 0; ///< Optional checksum for validation (0 if unused).
};

/// @brief A compiled bytecode module containing all data needed for execution.
/// @details The BytecodeModule is the top-level container produced by
///          BytecodeCompiler and consumed by BytecodeVM. It holds the constant
///          pools (i64, f64, string), compiled functions, native function
///          references, global variable descriptors, and optional debug info.
///
///          Constant pool entries are deduplicated: adding the same value twice
///          returns the same pool index. Function and native function references
///          are indexed by name for O(1) lookup.
struct BytecodeModule {
    // Header
    uint32_t magic;   ///< Module magic number (must equal kBytecodeModuleMagic).
    uint32_t version; ///< Bytecode format version (must equal kBytecodeVersion).
    uint32_t flags;   ///< Feature flags (reserved for future use).

    // Constant pools
    std::vector<int64_t> i64Pool;        ///< Deduplicated pool of 64-bit integer constants.
    std::vector<double> f64Pool;         ///< Deduplicated pool of 64-bit floating-point constants.
    std::vector<std::string> stringPool; ///< Deduplicated pool of string constants.

    // Functions
    std::vector<BytecodeFunction> functions;                 ///< All compiled bytecode functions.
    std::unordered_map<std::string, uint32_t> functionIndex; ///< Function name to index mapping.

    // Native function references
    std::vector<NativeFuncRef> nativeFuncs; ///< Native function descriptors.
    std::unordered_map<std::string, uint32_t>
        nativeFuncIndex; ///< Native function signature key to index mapping.

    // Globals
    std::vector<GlobalInfo> globals;                       ///< Global variable descriptors.
    std::unordered_map<std::string, uint32_t> globalIndex; ///< Global name to index mapping.

    // Debug info (optional)
    std::vector<SourceFileInfo> sourceFiles; ///< Source file references for debug info.

    /// @brief Construct a new BytecodeModule with default header values.
    /// @details Initializes magic to kBytecodeModuleMagic, version to
    ///          kBytecodeVersion, and flags to 0.
    BytecodeModule() : magic(kBytecodeModuleMagic), version(kBytecodeVersion), flags(0) {}

    /// @brief Find a compiled function by its fully qualified name.
    /// @param name The function name to search for.
    /// @return Borrowed pointer to the indexed function when both map and vector
    ///         entry agree with @p name; otherwise null.
    /// @warning A non-null pointer may be invalidated by later function-vector
    ///          reallocation.
    const BytecodeFunction *findFunction(const std::string &name) const {
        auto it = functionIndex.find(name);
        if (it != functionIndex.end() && it->second < functions.size() &&
            functions[it->second].name == name) {
            return &functions[it->second];
        }
        return nullptr;
    }

    /// @brief Add a compiled function to the module and update the name index.
    /// @param fn The BytecodeFunction to add (moved into the module).
    /// @return Existing index for an already indexed matching name, or the index
    ///         of the newly appended function.
    /// @throws std::length_error When a new entry exceeds 32-bit indexing.
    uint32_t addFunction(BytecodeFunction fn) {
        auto existing = functionIndex.find(fn.name);
        if (existing != functionIndex.end() && existing->second < functions.size() &&
            functions[existing->second].name == fn.name) {
            return existing->second;
        }
        if (functions.size() > std::numeric_limits<uint32_t>::max())
            throw std::length_error("bytecode function table exceeds 32-bit index width");
        uint32_t idx = static_cast<uint32_t>(functions.size());
        functionIndex[fn.name] = idx;
        functions.push_back(std::move(fn));
        return idx;
    }

    /// @brief Add a 64-bit integer constant to the pool, deduplicating by value.
    /// @param value The integer constant to add.
    /// @return The pool index of the (possibly pre-existing) constant.
    /// @throws std::length_error When a new entry exceeds 32-bit indexing.
    uint32_t addI64(int64_t value) {
        return detail::findOrAddToPool(i64Pool, value, std::equal_to<int64_t>{});
    }

    /// @brief Add a 64-bit floating-point constant to the pool, deduplicating by bitwise
    /// comparison.
    /// @details Uses bitwise comparison (not IEEE equality) so that distinct NaN
    ///          representations are stored separately while +0.0 and -0.0 remain distinct.
    /// @param value The double constant to add.
    /// @return The pool index of the (possibly pre-existing) constant.
    /// @throws std::length_error When a new entry exceeds 32-bit indexing.
    uint32_t addF64(double value) {
        auto bitwiseEq = [](double a, double b) {
            uint64_t pa = 0;
            uint64_t pb = 0;
            std::memcpy(&pa, &a, sizeof(pa));
            std::memcpy(&pb, &b, sizeof(pb));
            return pa == pb;
        };
        return detail::findOrAddToPool(f64Pool, value, bitwiseEq);
    }

    /// @brief Add a string constant to the pool, deduplicating by value.
    /// @param value The string constant to add.
    /// @return The pool index of the (possibly pre-existing) string.
    /// @throws std::length_error When a new entry exceeds 32-bit indexing.
    uint32_t addString(const std::string &value) {
        return detail::findOrAddToPool(stringPool, value, std::equal_to<std::string>{});
    }

    /// @brief Add a native function reference, deduplicating by call signature.
    /// @details If a native function with the same name and call-site shape is
    ///          already registered, returns the existing index. The same name
    ///          with a different arity or return expectation receives a distinct
    ///          entry so CALL_NATIVE arity validation cannot reuse stale metadata.
    /// @param name       The fully qualified native function name.
    /// @param paramCount Number of parameters the function expects.
    /// @param hasReturn  True if the function returns a value.
    /// @return The index of the native function reference.
    /// @throws std::length_error When a new entry exceeds 32-bit indexing.
    /// @post A new record caches runtime descriptor/signature and string
    ///       ownership traits when available.
    uint32_t addNativeFunc(const std::string &name, uint32_t paramCount, bool hasReturn) {
        const std::string key = detail::nativeFunctionKey(name, paramCount, hasReturn);
        auto it = nativeFuncIndex.find(key);
        if (it != nativeFuncIndex.end()) {
            return it->second;
        }
        if (nativeFuncs.size() > std::numeric_limits<uint32_t>::max())
            throw std::length_error("bytecode native table exceeds 32-bit index width");
        uint32_t idx = static_cast<uint32_t>(nativeFuncs.size());
        nativeFuncIndex[key] = idx;
        NativeFuncRef ref;
        ref.name = name;
        ref.paramCount = paramCount;
        ref.hasReturn = hasReturn;
        ref.runtimeDescriptor = il::runtime::findRuntimeDescriptor(name);
        ref.runtimeSignature = ref.runtimeDescriptor ? &ref.runtimeDescriptor->signature
                                                     : il::runtime::findRuntimeSignature(name);
        ref.consumesClonedStringArgs = name == "rt_str_concat" || name == "Zanna.String.Concat";
        ref.consumesOwnedStringArgs = name == "rt_str_release_maybe";
        ref.returnsString =
            ref.runtimeSignature && ref.runtimeSignature->retType.kind == il::core::Type::Kind::Str;
        nativeFuncs.push_back(std::move(ref));
        return idx;
    }

    /// @brief Add a global variable descriptor, deduplicating by name.
    /// @param global The global descriptor to add.
    /// @return Existing index for an already indexed matching name, or the index
    ///         of the newly appended descriptor.
    /// @throws std::length_error When a new entry exceeds 32-bit indexing.
    uint32_t addGlobal(GlobalInfo global) {
        auto it = globalIndex.find(global.name);
        if (it != globalIndex.end() && it->second < globals.size() &&
            globals[it->second].name == global.name)
            return it->second;

        if (globals.size() > std::numeric_limits<uint32_t>::max())
            throw std::length_error("bytecode global table exceeds 32-bit index width");
        uint32_t idx = static_cast<uint32_t>(globals.size());
        globalIndex[global.name] = idx;
        globals.push_back(std::move(global));
        return idx;
    }
};

} // namespace bytecode
} // namespace zanna
