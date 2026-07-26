//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/sem/OverloadResolution.cpp
// Purpose: Resolve user-class method and property-accessor overloads using
//          inheritance-aware candidate collection and deterministic conversion
//          ranking.
// Key invariants:
//   * Derived declarations shadow same-named base declarations.
//   * Only exact matches and INTEGER-to-DOUBLE widening are viable.
//   * Private candidates are visible only from their declaring class.
// Ownership: Resolution results borrow class and method metadata from OopIndex;
//            diagnostic text and returned name strings are owned by the result.
// References: docs/internals/codemap/basic.md
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Implements BASIC user-defined method overload resolution.
/// @details Collects candidates through the inheritance chain, filters them by
///          access and receiver kind, and ranks viable signatures.
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/sem/OverloadResolution.hpp"

#include "frontends/basic/StringUtils.hpp"
#include "frontends/basic/ast/NodeFwd.hpp"

namespace il::frontends::basic::sem {

namespace {
/// @brief True if an argument type exactly matches the expected parameter type.
/// @param expect Parameter type declared by the candidate signature.
/// @param got Argument type inferred at the call site.
/// @return True when both canonical BASIC types are identical.
static inline bool isExactMatch(::il::frontends::basic::Type expect,
                                ::il::frontends::basic::Type got) noexcept {
    return expect == got;
}

/// @brief True if @p got widens to @p expect via the only allowed numeric promotion (I64→F64).
/// @details Integer widening is already canonicalized to I64, so the sole widening here is
///          passing an INTEGER (I64) argument to a DOUBLE (F64) parameter.
/// @param expect Parameter type declared by the candidate signature.
/// @param got Argument type inferred at the call site.
/// @return True only for the canonical I64-to-F64 widening conversion.
static inline bool isWideningAllowed(::il::frontends::basic::Type expect,
                                     ::il::frontends::basic::Type got) noexcept {
    // Only numeric widening: int->float64; integer to integer widening already canonicalized to
    // I64. For this frontend, INT maps to I64 and SINGLE/DOUBLE to F64; allow I64->F64.
    return (expect == ::il::frontends::basic::Type::F64 &&
            got == ::il::frontends::basic::Type::I64);
}

/// @brief Format a method signature as `Class.name(TYPE, ...)` for diagnostic messages.
/// @param qclass Qualified class name to prefix to the signature.
/// @param name Method or synthesized accessor name.
/// @param mi Candidate metadata containing the parameter types.
/// @return A BASIC-oriented signature string suitable for diagnostics.
static inline std::string signatureText(std::string_view qclass,
                                        std::string_view name,
                                        const ClassInfo::MethodInfo &mi) {
    std::string s;
    s.reserve(64);
    s += std::string(qclass);
    s += ".";
    s += std::string(name);
    s += "(";
    for (size_t i = 0; i < mi.sig.paramTypes.size(); ++i) {
        if (i)
            s += ", ";
        auto t = mi.sig.paramTypes[i];
        switch (t) {
            case Type::I64:
                s += "INTEGER";
                break;
            case Type::F64:
                s += "DOUBLE";
                break;
            case Type::Str:
                s += "STRING";
                break;
            case Type::Bool:
                s += "BOOLEAN";
                break;
        }
    }
    s += ")";
    return s;
}
} // namespace

/// @brief Resolve a method (or property accessor) overload on a class.
/// @param index OOP index used to look up classes and walk inheritance.
/// @param qualifiedClass Qualified name of the receiver class.
/// @param methodName Method name being called (also matched against `get_`/`set_` accessors).
/// @param isStatic Whether a static or instance method is required.
/// @param argTypes Argument types at the call site.
/// @param currentClass Calling class (for private-access checks).
/// @param de Optional diagnostic emitter for no-match/ambiguous errors.
/// @param loc Call-site location for diagnostics.
/// @return The resolved method (with its declaring class for dispatch), or nullopt on failure.
/// @details Collects candidates across the inheritance chain (most-derived first, honoring
///          shadowing), filters by static-ness and private access, then ranks viable
///          arity-matching overloads — exact parameter matches outrank I64→F64 widening.
///          Reports E_OVERLOAD_NO_MATCH when nothing fits and E_OVERLOAD_AMBIGUOUS on a tie.
std::optional<ResolvedMethod> resolveMethodOverload(const OopIndex &index,
                                                    std::string_view qualifiedClass,
                                                    std::string_view methodName,
                                                    bool isStatic,
                                                    const std::vector<Type> &argTypes,
                                                    std::string_view currentClass,
                                                    DiagnosticEmitter *de,
                                                    il::support::SourceLoc loc) {
    // Normalize class casing using index metadata
    const ClassInfo *ci = index.findClass(std::string(qualifiedClass));
    if (!ci)
        return std::nullopt;

    // Build candidate list: methodName plus property accessors matching arity.
    // BUG-OOP-002/003 fix: Walk the inheritance hierarchy to find methods.
    struct Cand {
        const ClassInfo::MethodInfo *mi{nullptr};
        std::string name;
        std::string declaringClass; // Class where method is defined (for mangling)
    };

    std::vector<Cand> cands;

    /// @brief Adds non-shadowed candidates declared by one class.
    /// @param classInfo Class whose methods are inspected.
    auto addFromClass = [&](const ClassInfo *classInfo) {
        /// @brief Adds a named method unless a derived class already supplied it.
        /// @param name Method or property-accessor name.
        auto addIf = [&](const std::string &name) {
            auto it = classInfo->methods.find(name);
            if (it != classInfo->methods.end()) {
                // Check if we already have a candidate with this name (shadowing)
                bool alreadyHave = false;
                for (const auto &c : cands) {
                    if (c.name == name) {
                        alreadyHave = true;
                        break;
                    }
                }
                if (!alreadyHave)
                    cands.push_back(Cand{&it->second, name, classInfo->qualifiedName});
            }
        };
        addIf(std::string(methodName));
        // Properties: get_Name has 0 user params; set_Name has 1 user param.
        if (argTypes.size() == 0)
            addIf("get_" + std::string(methodName));
        if (argTypes.size() == 1)
            addIf("set_" + std::string(methodName));
    };

    // Walk inheritance hierarchy from most derived to base
    const ClassInfo *cur = ci;
    while (cur) {
        addFromClass(cur);
        if (cur->baseQualified.empty())
            break;
        cur = index.findClass(cur->baseQualified);
    }

    // Filter: static/instance and access control
    std::vector<Cand> filtered;
    filtered.reserve(cands.size());
    for (const auto &c : cands) {
        if (c.mi->isStatic != isStatic)
            continue;
        // Private methods can only be accessed from the declaring class
        if (c.mi->sig.access == Access::Private && c.declaringClass != currentClass)
            continue;
        filtered.push_back(c);
    }

    if (filtered.empty()) {
        if (de) {
            std::string msg = "no matching overload for '" + std::string(methodName) + "(";
            for (size_t i = 0; i < argTypes.size(); ++i) {
                if (i)
                    msg += ", ";
                switch (argTypes[i]) {
                    case Type::I64:
                        msg += "INTEGER";
                        break;
                    case Type::F64:
                        msg += "DOUBLE";
                        break;
                    case Type::Str:
                        msg += "STRING";
                        break;
                    case Type::Bool:
                        msg += "BOOLEAN";
                        break;
                }
            }
            msg += ")'";
            de->emit(il::support::Severity::Error,
                     "E_OVERLOAD_NO_MATCH",
                     loc,
                     static_cast<uint32_t>(methodName.size()),
                     std::move(msg));
        }
        return std::nullopt;
    }

    // Rank: exact match wins; else allow widening numeric conversion (I64->F64) per param.
    int bestScore = -1;
    std::vector<size_t> bestIdx;
    for (size_t i = 0; i < filtered.size(); ++i) {
        const auto &mi = *filtered[i].mi;
        if (mi.sig.paramTypes.size() != argTypes.size())
            continue;
        int score = 0;
        bool viable = true;
        for (size_t p = 0; p < argTypes.size(); ++p) {
            if (isExactMatch(mi.sig.paramTypes[p], argTypes[p])) {
                score += 2;
                continue;
            }
            if (isWideningAllowed(mi.sig.paramTypes[p], argTypes[p])) {
                score += 1;
                continue;
            }
            viable = false; // narrowing or incompatible
            break;
        }
        if (!viable)
            continue;
        if (score > bestScore) {
            bestScore = score;
            bestIdx.clear();
            bestIdx.push_back(i);
        } else if (score == bestScore) {
            bestIdx.push_back(i);
        }
    }

    if (bestIdx.empty()) {
        if (de) {
            std::string msg = "no viable overload for '" + std::string(methodName) + "'";
            de->emit(il::support::Severity::Error,
                     "E_OVERLOAD_NO_MATCH",
                     loc,
                     static_cast<uint32_t>(methodName.size()),
                     std::move(msg));
        }
        return std::nullopt;
    }
    if (bestIdx.size() > 1) {
        if (de) {
            std::string msg = "ambiguous call to '" + std::string(methodName) + "' among: ";
            bool first = true;
            for (size_t i : bestIdx) {
                if (!first)
                    msg += "; ";
                first = false;
                msg += signatureText(ci->qualifiedName, filtered[i].name, *filtered[i].mi);
            }
            de->emit(il::support::Severity::Error,
                     "E_OVERLOAD_AMBIGUOUS",
                     loc,
                     static_cast<uint32_t>(methodName.size()),
                     std::move(msg));
        }
        return std::nullopt;
    }

    const auto &win = filtered[bestIdx.front()];
    // BUG-OOP-002/003 fix: Return the declaring class for proper method dispatch
    return ResolvedMethod{ci, win.mi, win.declaringClass, win.name};
}

} // namespace il::frontends::basic::sem
