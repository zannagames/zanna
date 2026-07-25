//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Sema_Completion.cpp
/// @brief Completion and tooling query APIs for the Zia semantic analyzer.
///
/// @details Implements the read-only query methods on `Sema` that are used
/// by IDE tooling — in particular the code-completion engine in Phase 2.
/// All methods here are const and only read the symbol tables that were
/// built during analyze(); they never mutate analyzer state.
///
/// ## Implemented APIs
///
/// - `getGlobalSymbols()`   — all symbols in the global (module-level) scope
/// - `getMembersOf(type)`   — fields + methods of a user-defined type, or
///                            delegates to getRuntimeMembers() for Ptr types
/// - `getRuntimeMembers(cls)` — methods + properties from the RuntimeRegistry
/// - `getTypeNames()`       — names of all class/struct/interface declarations
/// - `getBoundModuleNames()` — short aliases from `bind Namespace as Alias;`
/// - `getModuleExports(mod)` — exported symbols of a bound file module
/// - `getBoundFileModuleNames()` — file-module roots usable for `Module.`
///
/// @see Sema.hpp for declarations and documentation.
/// @see ZiaAnalysis.hpp for parseAndAnalyze(), which creates the Sema object.
///
//===----------------------------------------------------------------------===//

#include "frontends/common/RuntimeMethodResolver.hpp"
#include "frontends/zia/RuntimeAdapter.hpp"
#include "frontends/zia/Sema.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"
#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace il::frontends::zia {

namespace {

/// @brief Extract the parameter names from a parameter list (for signature/tooling display).
/// @param params Source parameters in declaration order.
/// @return Parameter-name vector preserving declaration order.
std::vector<std::string> paramNamesFor(const std::vector<Param> &params) {
    std::vector<std::string> names;
    names.reserve(params.size());
    for (const auto &param : params)
        names.push_back(param.name);
    return names;
}

/// @brief Order two source locations by (file, line, column).
/// @param a Left location.
/// @param b Right location.
/// @return -1 if @p a precedes @p b, 1 if it follows, 0 if equal.
int compareToolLoc(const SourceLoc &a, const SourceLoc &b) {
    if (a.file_id != b.file_id)
        return a.file_id < b.file_id ? -1 : 1;
    if (a.line != b.line)
        return a.line < b.line ? -1 : 1;
    if (a.column != b.column)
        return a.column < b.column ? -1 : 1;
    return 0;
}

/// @brief Produce a user-facing spelling for a semantic type.
/// @param type Type to render.
/// @return Display spelling, or `Unknown` for a null type.
std::string displayType(const TypeRef &type) {
    return type ? type->toDisplayString() : "Unknown";
}

/// @brief Format a runtime callable as a Zia-style tooling signature.
/// @param name Source-visible callable name.
/// @param type Semantic function type.
/// @param paramNames Registry parameter names, which may be incomplete.
/// @return `name(param: Type) -> Result`, or @p name alone for a non-function type.
std::string runtimeCallableSignature(const std::string &name,
                                     const TypeRef &type,
                                     const std::vector<std::string> &paramNames) {
    if (!type || type->kind != TypeKindSem::Function)
        return name;

    std::ostringstream out;
    out << name << "(";
    auto params = type->paramTypes();
    for (size_t i = 0; i < params.size(); ++i) {
        if (i > 0)
            out << ", ";
        if (i < paramNames.size() && !paramNames[i].empty())
            out << paramNames[i];
        else
            out << "arg" << (i + 1);
        out << ": " << displayType(params[i]);
    }
    out << ") -> " << displayType(type->returnType());
    return out.str();
}

/// @brief Adapt extern-function parameter names to a runtime member's exposed arity.
/// @param externSym Backing extern symbol, if available.
/// @param memberArity Number of parameters visible on the member.
/// @return Matching names, names with a hidden receiver removed, or an empty vector.
std::vector<std::string> memberParamNamesFromExtern(const Symbol *externSym, size_t memberArity) {
    if (!externSym || externSym->paramNames.empty())
        return {};
    if (externSym->paramNames.size() == memberArity)
        return externSym->paramNames;
    if (externSym->paramNames.size() == memberArity + 1)
        return {externSym->paramNames.begin() + 1, externSym->paramNames.end()};
    return {};
}

/// @brief Look up curated prose for prominent runtime members.
/// @param className Fully qualified runtime class.
/// @param memberName Source-visible member name.
/// @return Static authored text, or nullptr when only generated documentation is available.
const char *authoredRuntimeMemberDocumentation(const std::string &className,
                                               const std::string &memberName) {
    struct Doc {
        const char *className;
        const char *memberName;
        const char *text;
    };

    static constexpr Doc docs[] = {
        {"Zanna.Terminal",
         "Say",
         "Writes text followed by a newline to the terminal output stream."},
        {"Zanna.Terminal",
         "Print",
         "Writes text to the terminal output stream without appending a newline."},
        {"Zanna.Terminal",
         "Ask",
         "Prompts the user and returns the entered text when input is available."},
        {"Zanna.IO.File", "ReadAllText", "Reads an entire text file into a String."},
        {"Zanna.IO.File",
         "WriteAllText",
         "Writes a String to a file, replacing the previous contents."},
        {"Zanna.IO.File", "Exists", "Returns whether a file exists at the supplied path."},
        {"Zanna.IO.Path", "Join", "Combines path fragments using the platform path separator."},
        {"Zanna.GUI.App", "Poll", "Processes pending window and input events for the app."},
        {"Zanna.GUI.App",
         "Render",
         "Lays out and paints the current GUI frame when the app needs repainting."},
        {"Zanna.GUI.CodeEditor",
         "SetText",
         "Replaces the editor buffer text and refreshes editor state."},
        {"Zanna.GUI.CodeEditor", "GetText", "Returns the current editor buffer text."},
        {"Zanna.GUI.CodeEditor",
         "Text",
         "Read-only property containing the current editor buffer text."},
        {"Zanna.GUI.CodeEditor",
         "Revision",
         "Read-only counter that changes when the editor buffer changes."},
        {"Zanna.GUI.App",
         "Root",
         "Root widget for the app window; add top-level layout containers here."},
    };

    for (const auto &doc : docs) {
        if (className == doc.className && memberName == doc.memberName)
            return doc.text;
    }
    return nullptr;
}

/// @brief Build hover documentation for a runtime method.
/// @param className Fully qualified runtime class.
/// @param methodName Source-visible method name.
/// @param target Lowered runtime target, if one is registered.
/// @param type Semantic callable type.
/// @param paramNames Exposed parameter names.
/// @return Authored text when available followed by generated signature and target details.
std::string runtimeMethodDocumentation(const std::string &className,
                                       const std::string &methodName,
                                       const char *target,
                                       const TypeRef &type,
                                       const std::vector<std::string> &paramNames) {
    std::ostringstream out;
    if (const char *authored = authoredRuntimeMemberDocumentation(className, methodName))
        out << authored << "\n";
    out << "Runtime method " << className << "." << methodName << ".\n";
    out << "Signature: " << runtimeCallableSignature(methodName, type, paramNames);
    if (target && *target)
        out << "\nTarget: " << target;
    return out.str();
}

/// @brief Build hover documentation for a runtime property.
/// @param className Fully qualified runtime class.
/// @param prop Runtime property metadata.
/// @param type Semantic property type.
/// @return Authored text when available followed by mutability, type, and accessor details.
std::string runtimePropertyDocumentation(const std::string &className,
                                         const il::runtime::RuntimeProperty &prop,
                                         const TypeRef &type) {
    std::ostringstream out;
    if (const char *authored =
            authoredRuntimeMemberDocumentation(className, prop.name ? prop.name : ""))
        out << authored << "\n";
    out << "Runtime property " << className << "." << (prop.name ? prop.name : "");
    if (prop.readonly)
        out << " (read-only)";
    out << ".\nType: " << displayType(type);
    if (prop.getter && *prop.getter)
        out << "\nGetter: " << prop.getter;
    if (prop.setter && *prop.setter)
        out << "\nSetter: " << prop.setter;
    return out.str();
}

} // namespace

// ---------------------------------------------------------------------------
// getGlobalSymbols
// ---------------------------------------------------------------------------

/// @brief Return all symbols in the global (module-level) scope.
/// @return Top-level functions, type constructors, bound runtime identifiers, and globals.
/// @details Reads `scopes_[0]` (the module scope built during analyze()); function-local
///          variables are not present, having been popped when their blocks were analyzed.
std::vector<Symbol> Sema::getGlobalSymbols() const {
    std::vector<Symbol> result;
    if (scopes_.empty())
        return result;

    // scopes_[0] is the global (module-level) scope created in Sema::analyze().
    // It contains: top-level funcs, class/struct/interface ctors, bound runtime
    // identifiers, and global variables.  Local variables inside function bodies
    // were popped off the scope stack when their blocks were analyzed.
    for (const auto &[name, sym] : scopes_[0]->getSymbols()) {
        result.push_back(sym);
    }
    return result;
}

// ---------------------------------------------------------------------------
// getVisibleSymbolsAtPosition
// ---------------------------------------------------------------------------

/// @brief Return symbols visible at a source position, innermost scope first, for completion.
/// @param fileId File of the cursor (0 matches any file).
/// @param line Cursor line.
/// @param col Cursor column.
/// @return Deduplicated visible symbols ordered by enclosing-scope depth (deepest first), then
///         by declaration position.
/// @details Considers each scoped symbol whose declaration precedes the cursor and whose
///          enclosing scope (from @c scopeSnapshots_) contains the cursor; shadowing is handled
///          by keeping only the first (deepest/nearest) symbol seen for each name.
std::vector<Symbol> Sema::getVisibleSymbolsAtPosition(uint32_t fileId,
                                                      uint32_t line,
                                                      uint32_t col) const {
    struct Candidate {
        Symbol symbol;
        SourceLoc loc;
        size_t depth{0};
    };

    std::vector<Candidate> candidates;
    const SourceLoc cursor{fileId, line, col};
    for (const auto &ss : scopedSymbols_) {
        if (!ss.loc.isValid())
            continue;
        if (fileId != 0 && ss.loc.file_id != fileId)
            continue;
        if (compareToolLoc(ss.loc, cursor) > 0)
            continue;

        size_t depth = 0;
        auto scopeIt = scopeSnapshots_.find(ss.scopeId);
        if (scopeIt != scopeSnapshots_.end()) {
            const auto &scope = scopeIt->second;
            if (fileId != 0 && scope.startLoc.hasFile() && scope.startLoc.file_id != fileId)
                continue;
            if (scope.startLoc.isValid() && compareToolLoc(scope.startLoc, cursor) > 0)
                continue;
            if (scope.endLoc.isValid() && cursor.file_id == scope.endLoc.file_id &&
                cursor.line > scope.endLoc.line)
                continue;
            depth = scope.depth;
        }

        candidates.push_back({ss.symbol, ss.loc, depth});
    }

    std::stable_sort(
        candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
            if (a.depth != b.depth)
                return a.depth > b.depth;
            return compareToolLoc(a.loc, b.loc) > 0;
        });

    std::vector<Symbol> result;
    std::unordered_set<std::string> seen;
    result.reserve(candidates.size());
    for (const auto &candidate : candidates) {
        if (seen.insert(candidate.symbol.name).second)
            result.push_back(candidate.symbol);
    }
    return result;
}

/// @brief Reify every source overload of a function as a tooling symbol.
/// @param name Semantic function-family name.
/// @return Function symbols carrying declaration locations, callable types, and parameter names.
std::vector<Symbol> Sema::getFunctionOverloadSymbols(const std::string &name) const {
    std::vector<Symbol> result;
    for (auto *decl : getFunctionOverloads(name)) {
        if (!decl)
            continue;
        Symbol sym;
        sym.kind = Symbol::Kind::Function;
        sym.name = decl->name;
        sym.type = functionTypeForDecl(*decl);
        sym.decl = decl;
        sym.loc = decl->loc;
        sym.isExported = decl->isExported;
        sym.paramNames = paramNamesFor(decl->params);
        result.push_back(std::move(sym));
    }
    return result;
}

// ---------------------------------------------------------------------------
// getMembersOf
// ---------------------------------------------------------------------------

/// @brief Return the members (fields and methods) of a type, for completion.
/// @param type The receiver type.
/// @return Member symbols; empty for non-aggregate types.
/// @details Runtime (Ptr) classes delegate to getRuntimeMembers(). User-defined
///          class/struct/interface members are gathered from @c fieldTypes_/@c methodTypes_,
///          which are keyed `TypeName.member`.
std::vector<Symbol> Sema::getMembersOf(const TypeRef &type) const {
    std::vector<Symbol> result;
    if (!type)
        return result;

    // For runtime class pointer types, delegate to getRuntimeMembers().
    if (type->kind == TypeKindSem::Ptr && !type->name.empty()) {
        return getRuntimeMembers(type->name);
    }

    // For user-defined types, look up in fieldTypes_ and methodTypes_ using
    // "TypeName.memberName" key format (established by Sema_Decl.cpp).
    if (type->kind != TypeKindSem::Class && type->kind != TypeKindSem::Struct &&
        type->kind != TypeKindSem::Interface) {
        return result;
    }

    const std::string &typeName = type->name;
    if (typeName.empty())
        return result;

    const std::string prefix = typeName + ".";

    for (const auto &[key, fieldType] : fieldTypes_) {
        if (key.rfind(prefix, 0) == 0) {
            std::string memberName = key.substr(prefix.size());
            Symbol sym;
            sym.kind = Symbol::Kind::Field;
            sym.name = memberName;
            sym.type = fieldType;
            result.push_back(sym);
        }
    }

    for (const auto &[key, methodType] : methodTypes_) {
        if (key.rfind(prefix, 0) == 0) {
            std::string memberName = key.substr(prefix.size());
            std::unordered_set<std::string> emitted;
            for (auto *decl : collectMethodOverloads(typeName, memberName, true)) {
                if (!decl)
                    continue;
                TypeRef overloadType = methodTypeForDecl(*decl);
                std::string sigKey = memberName;
                if (overloadType && overloadType->kind == TypeKindSem::Function)
                    sigKey += "#" + overloadType->toDisplayString();
                if (!emitted.insert(sigKey).second)
                    continue;

                Symbol sym;
                sym.kind = Symbol::Kind::Method;
                sym.name = memberName;
                sym.type = overloadType ? overloadType : methodType;
                sym.paramNames = paramNamesFor(decl->params);
                result.push_back(std::move(sym));
            }
            if (emitted.empty()) {
                Symbol sym;
                sym.kind = Symbol::Kind::Method;
                sym.name = memberName;
                sym.type = methodType;
                result.push_back(sym);
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// getRuntimeMembers
// ---------------------------------------------------------------------------

/// @brief Return the methods and properties of a runtime class, for completion.
/// @param className Qualified runtime class name (e.g. `Zanna.GUI.Canvas`).
/// @return Member symbols (methods as functions, properties as fields); empty if unknown.
/// @details Looks the class up in the RuntimeRegistry catalog and converts each method's
///          parsed signature and each property's IL type token into Zia type references.
std::vector<Symbol> Sema::getRuntimeMembers(const std::string &className) const {
    std::vector<Symbol> result;
    const auto &catalog = il::runtime::RuntimeRegistry::instance().rawCatalog();

    const il::runtime::RuntimeClass *rtClass = nullptr;
    for (const auto &cls : catalog) {
        if (cls.qname && cls.qname == className) {
            rtClass = &cls;
            break;
        }
    }

    if (!rtClass)
        return result;

    std::unordered_set<std::string> emittedMembers;

    auto appendRuntimeMethods = [&](const il::runtime::RuntimeClass *sourceClass) {
        if (!sourceClass)
            return;

        for (const auto &method : sourceClass->methods) {
            if (!method.name)
                continue;

            auto sig = il::runtime::parseRuntimeSignature(method.signature ? method.signature : "");
            TypeRef retType = sig.isValid() ? toZiaReturnType(sig) : types::unknown();

            std::vector<TypeRef> paramTypes;
            if (sig.isValid()) {
                for (auto ilType : sig.params) {
                    paramTypes.push_back(toZiaType(ilType));
                }
            }

            Symbol sym;
            sym.kind = Symbol::Kind::Method;
            sym.name = method.name;
            sym.type = types::function(std::move(paramTypes), retType);
            sym.isExtern = true;

            std::string key = sym.name + "#" + (sym.type ? sym.type->toDisplayString() : "");
            if (!emittedMembers.insert(std::move(key)).second)
                continue;

            if (const Symbol *externSym = [&]() -> const Symbol * {
                    if (!method.target || scopes_.empty())
                        return nullptr;
                    const auto &symbols = scopes_[0]->getSymbols();
                    auto it = symbols.find(method.target);
                    return it == symbols.end() ? nullptr : &it->second;
                }()) {
                sym.paramNames = memberParamNamesFromExtern(
                    externSym, sym.type ? sym.type->paramTypes().size() : 0);
            }
            sym.documentation = runtimeMethodDocumentation(
                className, method.name, method.target, sym.type, sym.paramNames);
            result.push_back(sym);
        }
    };

    // Methods — parse the signature to build a function TypeRef.
    appendRuntimeMethods(rtClass);

    if (common::isGuiWidgetSubclass(className)) {
        for (const auto &cls : catalog) {
            if (cls.qname && std::string_view(cls.qname) == "Zanna.GUI.Widget") {
                appendRuntimeMethods(&cls);
                break;
            }
        }
    }

    // Properties — represent as Field symbols with the property's struct type.
    for (const auto &prop : rtClass->properties) {
        if (!prop.name)
            continue;

        auto ilType = il::runtime::mapILToken(prop.type ? prop.type : "");
        TypeRef propType = toZiaType(ilType);

        Symbol sym;
        sym.kind = Symbol::Kind::Field;
        sym.name = prop.name;
        sym.type = propType;
        sym.isFinal = prop.readonly;
        sym.isExtern = true;
        sym.documentation = runtimePropertyDocumentation(className, prop, sym.type);
        result.push_back(sym);
    }

    return result;
}

// ---------------------------------------------------------------------------
// getTypeNames
// ---------------------------------------------------------------------------

/// @brief Return the names of all declared classes, structs, and interfaces.
/// @return Semantic type names gathered from the nominal declaration registries.
std::vector<std::string> Sema::getTypeNames() const {
    std::vector<std::string> names;
    names.reserve(classDecls_.size() + structDecls_.size() + interfaceDecls_.size());

    for (const auto &[name, _] : classDecls_)
        names.push_back(name);
    for (const auto &[name, _] : structDecls_)
        names.push_back(name);
    for (const auto &[name, _] : interfaceDecls_)
        names.push_back(name);

    return names;
}

// ---------------------------------------------------------------------------
// getBoundModuleNames
// ---------------------------------------------------------------------------

/// @brief Return the short aliases introduced by `bind Namespace as Alias;`.
/// @return The alias identifiers users can type before `.` (keys of @c aliasToNamespace_).
std::vector<std::string> Sema::getBoundModuleNames() const {
    std::vector<std::string> names;
    // aliasToNamespace_ maps short alias → full namespace path.
    // The keys are the prefixes users can type before '.'.
    names.reserve(aliasToNamespace_.size());
    for (const auto &[alias, _] : aliasToNamespace_) {
        names.push_back(alias);
    }
    return names;
}

// ---------------------------------------------------------------------------
// getModuleExports
// ---------------------------------------------------------------------------

/// @brief Return the exported symbols of a bound file module.
/// @param moduleName Name of the bound module.
/// @return Its exported symbols, or empty if the module is unknown.
std::vector<Symbol> Sema::getModuleExports(const std::string &moduleName) const {
    std::vector<Symbol> result;
    auto it = moduleExports_.find(moduleName);
    if (it == moduleExports_.end())
        return result;

    result.reserve(it->second.size());
    for (const auto &[name, sym] : it->second) {
        result.push_back(sym);
    }
    return result;
}

// ---------------------------------------------------------------------------
// getBoundFileModuleNames
// ---------------------------------------------------------------------------

/// @brief Return the file-module root names usable as a `Module.` completion prefix.
/// @return Names whose exported symbol maps were built during bind analysis.
std::vector<std::string> Sema::getBoundFileModuleNames() const {
    std::vector<std::string> names;
    names.reserve(moduleExports_.size());
    for (const auto &[moduleName, _] : moduleExports_)
        names.push_back(moduleName);
    return names;
}

// ---------------------------------------------------------------------------
// resolveModuleAlias
// ---------------------------------------------------------------------------

/// @brief Resolve a bound alias to its full namespace path.
/// @param alias The short alias (e.g. from `bind Zanna.GUI as G;`).
/// @return The full namespace path, or an empty string if the alias is unknown.
std::string Sema::resolveModuleAlias(const std::string &alias) const {
    auto it = aliasToNamespace_.find(alias);
    if (it == aliasToNamespace_.end())
        return {};
    return it->second;
}

// ---------------------------------------------------------------------------
// getNamespaceClasses
// ---------------------------------------------------------------------------

/// @brief Return the immediate child class/sub-namespace names under a namespace prefix.
/// @param nsPrefix Namespace prefix (e.g. `Zanna.GUI`).
/// @return Distinct immediate children (e.g. `Canvas`, `App`), for `Namespace.` completion.
/// @details Scans the runtime catalog for qualified names beginning with `nsPrefix.` and
///          extracts the first identifier after the prefix, de-duplicating results.
std::vector<std::string> Sema::getNamespaceClasses(const std::string &nsPrefix) const {
    std::vector<std::string> result;
    const std::string nsWithDot = nsPrefix + ".";
    const auto &catalog = il::runtime::RuntimeRegistry::instance().rawCatalog();

    std::unordered_set<std::string> seen;
    for (const auto &cls : catalog) {
        if (!cls.qname)
            continue;
        std::string_view qname = cls.qname;
        if (qname.size() <= nsWithDot.size())
            continue;
        if (qname.substr(0, nsWithDot.size()) != nsWithDot)
            continue;

        // Extract the immediate child identifier.
        // e.g.  "Zanna.GUI.Canvas"       → "Canvas"
        //        "Zanna.GUI.App.Toolbar"  → "App"
        std::string rest(qname.substr(nsWithDot.size()));
        auto dotPos = rest.find('.');
        std::string childName = (dotPos != std::string::npos) ? rest.substr(0, dotPos) : rest;
        if (!childName.empty() && seen.insert(childName).second)
            result.push_back(childName);
    }
    return result;
}

} // namespace il::frontends::zia
