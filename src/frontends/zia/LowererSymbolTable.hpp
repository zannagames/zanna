//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file LowererSymbolTable.hpp
/// @brief Function- and module-scoped symbol storage for the Zia IL lowerer.
///
/// @details The table separates transient local SSA/type/slot bindings from
///          global constants, variables, and initializer values that persist
///          throughout module lowering. It stores lowered values and semantic
///          types but does not own the declarations to which names refer.
///
//===----------------------------------------------------------------------===//
#pragma once

#include "frontends/zia/Types.hpp"
#include "il/core/Value.hpp"
#include <string>
#include <unordered_map>

namespace il::frontends::zia {

/// @brief Name-keyed symbol storage for the Zia lowerer.
/// @details Holds function-scoped locals (SSA values), their semantic types, slot pointers
///          (for mutable/cross-block variables), and module-scoped global constants, variables,
///          and initializers. Locals/types/slots are cleared between functions via
///          clearFunctionScope(); globals persist for the whole module.
/// @invariant Local names are unique within a function scope.
/// @invariant Global constants are populated before any function body is lowered.
class LowererSymbolTable {
  public:
    using Value = il::core::Value;

    /// @brief Construct an empty symbol table.
    LowererSymbolTable() = default;

    /// @brief Bind a local name to an SSA value.
    /// @param name Function-scoped source name to define or replace.
    /// @param value Lowered SSA value associated with @p name.
    void defineLocal(const std::string &name, Value value) {
        locals_[name] = value;
    }

    /// @brief Look up a local by name.
    /// @param name Function-scoped source name to find.
    /// @return Pointer to the value, or nullptr if not defined.
    Value *lookupLocal(const std::string &name) {
        auto it = locals_.find(name);
        return it != locals_.end() ? &it->second : nullptr;
    }

    /// @brief Read-only access to all locals in scope.
    /// @return Map from source names to lowered SSA values.
    const std::unordered_map<std::string, Value> &locals() const {
        return locals_;
    }

    /// @brief Mutable access to all locals in scope.
    /// @return Modifiable map from source names to lowered SSA values.
    std::unordered_map<std::string, Value> &locals() {
        return locals_;
    }

    /// @brief Drop all locals (used when leaving a function).
    /// @post lookupLocal() fails for every previously bound local name.
    void clearLocals() {
        locals_.clear();
    }

    /// @brief Record the semantic type of a local.
    /// @param name Function-scoped source name to define or replace.
    /// @param type Semantic type inferred or declared for @p name.
    void setLocalType(const std::string &name, TypeRef type) {
        localTypes_[name] = type;
    }

    /// @brief Look up a local's semantic type.
    /// @param name Function-scoped source name to find.
    /// @return The type, or nullptr if unknown.
    TypeRef lookupLocalType(const std::string &name) const {
        auto it = localTypes_.find(name);
        return it != localTypes_.end() ? it->second : nullptr;
    }

    /// @brief Read-only access to all local types.
    /// @return Map from local source names to semantic types.
    const std::unordered_map<std::string, TypeRef> &localTypes() const {
        return localTypes_;
    }

    /// @brief Mutable access to all local types.
    /// @return Modifiable map from local source names to semantic types.
    std::unordered_map<std::string, TypeRef> &localTypes() {
        return localTypes_;
    }

    /// @brief Drop all local type records.
    /// @post lookupLocalType() returns null for every previously recorded name.
    void clearLocalTypes() {
        localTypes_.clear();
    }

    /// @brief Associate a name with its stack-slot pointer (mutable/cross-block variable).
    /// @param name Function-scoped variable name to define or replace.
    /// @param slot IL pointer to the variable's stack storage.
    void registerSlot(const std::string &name, Value slot) {
        slots_[name] = slot;
    }

    /// @brief Look up a variable's slot pointer.
    /// @param name Function-scoped variable name to find.
    /// @return Pointer to the slot value, or nullptr if the variable is not slot-backed.
    Value *lookupSlot(const std::string &name) {
        auto it = slots_.find(name);
        return it != slots_.end() ? &it->second : nullptr;
    }

    /// @brief Forget a variable's slot (e.g. when a scratch loop variable goes out of scope).
    /// @param name Function-scoped variable whose slot binding is removed.
    void removeSlot(const std::string &name) {
        slots_.erase(name);
    }

    /// @brief Read-only access to all slot bindings.
    /// @return Map from variable names to stack-slot pointer values.
    const std::unordered_map<std::string, Value> &slots() const {
        return slots_;
    }

    /// @brief Mutable access to all slot bindings.
    /// @return Modifiable map from variable names to stack-slot pointer values.
    std::unordered_map<std::string, Value> &slots() {
        return slots_;
    }

    /// @brief Drop all slot bindings.
    /// @post lookupSlot() fails for every previously slot-backed variable.
    void clearSlots() {
        slots_.clear();
    }

    /// @brief Record a module-level compile-time constant by (qualified) name.
    /// @param name Qualified or unqualified global constant name.
    /// @param value Lowered constant value to define or replace.
    void defineGlobalConstant(const std::string &name, Value value) {
        globalConstants_[name] = value;
    }

    /// @brief Look up a global constant.
    /// @param name Qualified or unqualified constant name to find.
    /// @return Pointer to the constant value, or nullptr if none exists.
    const Value *lookupGlobalConstant(const std::string &name) const {
        auto it = globalConstants_.find(name);
        return it != globalConstants_.end() ? &it->second : nullptr;
    }

    /// @brief Test whether a global constant with this name is defined.
    /// @param name Qualified or unqualified constant name to test.
    /// @return True when the global-constant map contains @p name.
    bool hasGlobalConstant(const std::string &name) const {
        return globalConstants_.count(name) > 0;
    }

    /// @brief Mutable access to all global constants.
    /// @return Modifiable map from global names to constant values.
    std::unordered_map<std::string, Value> &globalConstants() {
        return globalConstants_;
    }

    /// @brief Read-only access to all global constants.
    /// @return Map from global names to constant values.
    const std::unordered_map<std::string, Value> &globalConstants() const {
        return globalConstants_;
    }

    /// @brief Record a module-level mutable variable and its type.
    /// @param name Qualified or unqualified global variable name.
    /// @param type Semantic type of the global variable.
    void defineGlobalVariable(const std::string &name, TypeRef type) {
        globalVariables_[name] = type;
    }

    /// @brief Look up a global variable's type.
    /// @param name Qualified or unqualified variable name to find.
    /// @return The type, or nullptr if no such global exists.
    TypeRef lookupGlobalVariable(const std::string &name) const {
        auto it = globalVariables_.find(name);
        return it != globalVariables_.end() ? it->second : nullptr;
    }

    /// @brief Mutable access to all global variables.
    /// @return Modifiable map from global names to semantic types.
    std::unordered_map<std::string, TypeRef> &globalVariables() {
        return globalVariables_;
    }

    /// @brief Read-only access to all global variables.
    /// @return Map from global names to semantic types.
    const std::unordered_map<std::string, TypeRef> &globalVariables() const {
        return globalVariables_;
    }

    /// @brief Record the lowered initializer value for a global.
    /// @param name Qualified or unqualified global variable name.
    /// @param value Lowered value produced for the global initializer.
    void defineGlobalInitializer(const std::string &name, Value value) {
        globalInitializers_[name] = value;
    }

    /// @brief Look up a global's initializer value.
    /// @param name Qualified or unqualified global name to find.
    /// @return Pointer to the initializer value, or nullptr if none was recorded.
    const Value *lookupGlobalInitializer(const std::string &name) const {
        auto it = globalInitializers_.find(name);
        return it != globalInitializers_.end() ? &it->second : nullptr;
    }

    /// @brief Mutable access to all global initializers.
    /// @return Modifiable map from global names to lowered initializer values.
    std::unordered_map<std::string, Value> &globalInitializers() {
        return globalInitializers_;
    }

    /// @brief Read-only access to all global initializers.
    /// @return Map from global names to lowered initializer values.
    const std::unordered_map<std::string, Value> &globalInitializers() const {
        return globalInitializers_;
    }

    /// @brief Clear all function-scoped state (locals, local types, slots) between functions.
    /// @post Global constants, variables, and initializer values are unchanged.
    void clearFunctionScope() {
        locals_.clear();
        localTypes_.clear();
        slots_.clear();
    }

  private:
    /// Function-scoped source names mapped to current SSA values.
    std::unordered_map<std::string, Value> locals_;
    /// Function-scoped source names mapped to semantic types.
    std::unordered_map<std::string, TypeRef> localTypes_;
    /// Mutable or cross-block local names mapped to stack-slot pointers.
    std::unordered_map<std::string, Value> slots_;
    /// Module-scoped compile-time constants indexed by name.
    std::unordered_map<std::string, Value> globalConstants_;
    /// Module-scoped mutable variables indexed by name.
    std::unordered_map<std::string, TypeRef> globalVariables_;
    /// Lowered initializer values for module-scoped variables.
    std::unordered_map<std::string, Value> globalInitializers_;
};

} // namespace il::frontends::zia
