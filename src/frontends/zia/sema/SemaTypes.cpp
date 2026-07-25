//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file SemaTypes.cpp
/// @brief Scope symbol-table method implementations shared by the Zia semantic analyzer and editor
/// services.
///
/// @details This file was split out of Sema.cpp to keep semantic analysis
/// responsibilities navigable without changing the Sema public interface or
/// diagnostic behavior. Member functions remain declared in Sema.hpp.
///
/// @see frontends/zia/Sema.hpp
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/sema/SemaTypes.hpp"

#include <utility>

namespace il::frontends::zia {

/// @brief Define a symbol in the current scope.
/// @param name The symbol name to register.
/// @param symbol The symbol metadata to associate with the name.
/// @post Any previous local binding for @p name has been replaced.
void Scope::define(const std::string &name, Symbol symbol) {
    symbols_[name] = std::move(symbol);
}

/// @brief Look up a symbol by name, walking parent scopes.
/// @param name The symbol name to search for.
/// @return Pointer to the symbol if found, nullptr otherwise.
/// @details Returns the innermost matching binding. The pointer refers to
///          storage owned by a scope and remains valid until that scope's map
///          rehashes, erases the entry, or is destroyed.
Symbol *Scope::lookup(const std::string &name) {
    auto it = symbols_.find(name);
    if (it != symbols_.end())
        return &it->second;
    if (parent_)
        return parent_->lookup(name);
    return nullptr;
}

/// @brief Look up a symbol only in the current scope (no parent walk).
/// @param name The symbol name to search for.
/// @return Pointer to the symbol if found in this scope, nullptr otherwise.
/// @details The returned pointer has the same invalidation rules as the
///          underlying local symbol map.
Symbol *Scope::lookupLocal(const std::string &name) {
    auto it = symbols_.find(name);
    return it != symbols_.end() ? &it->second : nullptr;
}

} // namespace il::frontends::zia
