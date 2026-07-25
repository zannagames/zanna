//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/objfile/SymbolTable.cpp
// Purpose: Implementation of symbol table for object file generation.
// Key invariants:
//   - Index 0 is always the null entry
//   - findOrAdd creates External symbols for unknown names
// Ownership/Lifetime:
//   - See SymbolTable.hpp
// Links: codegen/common/objfile/SymbolTable.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file SymbolTable.cpp
 * @brief Implements encoder symbol insertion, definition replacement, and lookup.
 */

#include "codegen/common/objfile/SymbolTable.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace zanna::codegen::objfile {

/// @copydoc SymbolTable::SymbolTable
SymbolTable::SymbolTable() {
    // Index 0 is the null symbol (ELF requirement, harmless for other formats).
    symbols_.push_back(Symbol{"", SymbolBinding::Local, SymbolSection::Undefined, 0, 0});
}

/// @copydoc SymbolTable::add
uint32_t SymbolTable::add(Symbol sym) {
    if (symbols_.size() >= std::numeric_limits<uint32_t>::max())
        throw std::length_error("SymbolTable index exceeds 32-bit object-file field range");

    if (!sym.name.empty()) {
        auto it = nameIndex_.find(sym.name);
        if (it != nameIndex_.end()) {
            Symbol &existing = symbols_.at(it->second);

            if (existing.binding == SymbolBinding::External &&
                sym.binding == SymbolBinding::External) {
                return it->second;
            }

            if (existing.binding == SymbolBinding::External &&
                sym.binding != SymbolBinding::External) {
                const std::string oldName = existing.name;
                existing = std::move(sym);
                if (existing.name != oldName) {
                    nameIndex_.erase(oldName);
                    nameIndex_[existing.name] = it->second;
                }
                ambiguousNames_.erase(oldName);
                ambiguousNames_.erase(existing.name);
                return it->second;
            }

            if (sym.binding == SymbolBinding::Global || existing.binding == SymbolBinding::Global) {
                throw std::invalid_argument("SymbolTable: duplicate global symbol '" + sym.name +
                                            "'");
            }

            // Duplicate locals are legal because object formats can carry
            // multiple same-spelled local labels. Name-only lookup is no longer
            // safe for that spelling, so mark it ambiguous instead of silently
            // rebinding lookups to the newest entry.
            ambiguousNames_.insert(sym.name);
            nameIndex_.erase(it);
        } else if (ambiguousNames_.count(sym.name) != 0) {
            // Additional duplicate local with an already ambiguous spelling.
        }
    }

    auto index = static_cast<uint32_t>(symbols_.size());
    if (!sym.name.empty() && ambiguousNames_.count(sym.name) == 0)
        nameIndex_[sym.name] = index;
    symbols_.push_back(std::move(sym));
    return index;
}

/// @copydoc SymbolTable::findOrAdd
uint32_t SymbolTable::findOrAdd(const std::string &name) {
    if (ambiguousNames_.count(name) != 0) {
        throw std::invalid_argument("SymbolTable: ambiguous symbol name '" + name + "'");
    }
    auto it = nameIndex_.find(name);
    if (it != nameIndex_.end())
        return it->second;

    // Not found — declare as external.
    return add(Symbol{name, SymbolBinding::External, SymbolSection::Undefined, 0, 0});
}

/// @copydoc SymbolTable::find
uint32_t SymbolTable::find(const std::string &name) const {
    if (ambiguousNames_.count(name) != 0)
        return 0;
    auto it = nameIndex_.find(name);
    if (it == nameIndex_.end())
        return 0;
    return it->second;
}

/// @copydoc SymbolTable::reserve
void SymbolTable::reserve(size_t totalSymbols) {
    if (symbols_.capacity() < totalSymbols)
        symbols_.reserve(totalSymbols);
}

} // namespace zanna::codegen::objfile
