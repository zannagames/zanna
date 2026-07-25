//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/objfile/SymbolTable.hpp
// Purpose: Symbol table for object file generation. Manages symbol entries
//          with name, binding, section, and offset information.
// Key invariants:
//   - Index 0 is reserved for the null entry (ELF requirement)
//   - Names are stored as mapped C-level names (e.g., rt_print_i64)
//   - Platform-specific mangling (Mach-O underscore) is applied by writers
//   - All local symbols precede global symbols (required by ELF and Mach-O)
// Ownership/Lifetime:
//   - Value type, typically lives for the duration of binary emission
// Links: codegen/common/objfile/CodeSection.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file SymbolTable.hpp
 * @brief Defines encoder symbols and the indexed table consumed by object writers.
 *
 * The table reserves index zero, coalesces repeated external declarations,
 * upgrades externals to definitions, and permits duplicate local labels while
 * marking their spelling unsafe for name-only lookup.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zanna::codegen::objfile {

/// @brief Linkage and definition state of an encoder symbol.
enum class SymbolBinding : uint8_t {
    Local,    ///< File-local symbol (static function, local label).
    Global,   ///< Globally visible defined symbol (exported function).
    External, ///< Undefined symbol (imported from another object/library).
};

/// @brief Logical encoder section containing a symbol definition.
enum class SymbolSection : uint8_t {
    Text,      ///< .text section (machine code).
    Rodata,    ///< .rodata / __TEXT,__const section (read-only data).
    Data,      ///< .data / __DATA,__data section (writable initialized data).
    Undefined, ///< External symbol, not defined in this object.
};

/// @brief Format-neutral object symbol recorded during binary encoding.
struct Symbol {
    ///< Unmangled C-level or local-label name.
    std::string name;
    ///< Linkage and definition state.
    SymbolBinding binding = SymbolBinding::External;
    ///< Logical defining section, or undefined for externals.
    SymbolSection section = SymbolSection::Undefined;
    size_t offset = 0; ///< Byte offset within section (0 for External).
    size_t size = 0;   ///< Size of symbol (0 if unknown).
};

/// @brief Manages indexed symbols and unambiguous name lookup for object generation.
/// @details Index zero is the null symbol. Duplicate locals receive distinct
///          indices and make their spelling ambiguous; duplicate globals are
///          rejected, while an external may be replaced by its later definition.
class SymbolTable {
  public:
    /// @brief Construct a table containing only the required null symbol.
    SymbolTable();

    /// @brief Add or coalesce one symbol.
    /// @param sym Symbol value to insert or use as a replacing definition.
    /// @return Stable index of the inserted, reused, or upgraded entry.
    /// @throws std::length_error if a 32-bit index cannot be allocated.
    /// @throws std::invalid_argument for duplicate globals.
    uint32_t add(Symbol sym);

    /// @brief Find a symbol by unambiguous name or declare it external.
    /// @param name Name to locate or intern.
    /// @return Existing or new external symbol index.
    /// @throws std::invalid_argument when duplicate locals make @p name ambiguous.
    uint32_t findOrAdd(const std::string &name);

    /// @brief Look up an unambiguous symbol name.
    /// @param name Name to locate.
    /// @return Symbol index, or zero when absent or ambiguous.
    uint32_t find(const std::string &name) const;

    /// @brief Reserve backing storage without adding symbols.
    /// @param totalSymbols Desired capacity including the null entry.
    void reserve(size_t totalSymbols);

    /// @brief Access a symbol by index.
    /// @param index Symbol-table index.
    /// @return Immutable symbol reference.
    /// @throws std::out_of_range when @p index is invalid.
    const Symbol &at(uint32_t index) const {
        return symbols_.at(index);
    }

    /// @brief Access a mutable symbol by index.
    /// @param index Symbol-table index.
    /// @return Mutable symbol reference.
    /// @throws std::out_of_range when @p index is invalid.
    Symbol &at(uint32_t index) {
        return symbols_.at(index);
    }

    /// @brief Return the symbol count including the null entry.
    /// @return Number of indexed entries.
    /// @throws std::length_error if the backing vector exceeds 32-bit range.
    uint32_t count() const {
        if (symbols_.size() > std::numeric_limits<uint32_t>::max())
            throw std::length_error("SymbolTable count exceeds 32-bit object-file field range");
        return static_cast<uint32_t>(symbols_.size());
    }

    /// @brief Const iterator type for symbol-order traversal.
    using const_iterator = std::vector<Symbol>::const_iterator;

    /// @brief Return an iterator to the first symbol, including the null entry.
    /// @return Beginning const iterator.
    const_iterator begin() const {
        return symbols_.begin();
    }

    /// @brief Return the one-past-end symbol iterator.
    /// @return Ending const iterator.
    const_iterator end() const {
        return symbols_.end();
    }

    /// @brief Access all indexed symbols in storage order.
    /// @return Immutable backing vector.
    const std::vector<Symbol> &symbols() const {
        return symbols_;
    }

  private:
    std::vector<Symbol> symbols_;
    std::unordered_map<std::string, uint32_t> nameIndex_;
    std::unordered_set<std::string> ambiguousNames_;
};

} // namespace zanna::codegen::objfile
