//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
// File: src/frontends/basic/Semantic_OOP_Builder.cpp
// Purpose: Implements the phased BASIC class, interface, and enum metadata
//          builder used by semantic analysis and lowering.
// Key invariants:
//   - Declarations are indexed before inheritance and interface names resolve.
//   - Inheritance cycles are cut before recursive virtual-table construction.
//   - Base virtual tables are complete before derived slots are assigned.
//   - Invalid relationships emit diagnostics when possible and leave a safe
//     partial index rather than dangling metadata.
// Ownership/Lifetime:
//   - OopIndexBuilder borrows its destination index and optional emitter.
//   - AST declarations are read synchronously and are never retained by pointer.
// Links: src/frontends/basic/Semantic_OOP.cpp,
//        src/frontends/basic/Semantic_OOP_Helpers.cpp,
//        src/frontends/basic/detail/Semantic_OOP_Internal.hpp,
//        src/frontends/basic/OopIndex.hpp
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/IdentifierUtil.hpp"
#include "frontends/basic/SemanticDiagUtil.hpp"
#include "frontends/basic/SemanticDiagnostics.hpp"
#include "frontends/basic/StringUtils.hpp"
#include "frontends/basic/TypeSuffix.hpp"
#include "frontends/basic/detail/Semantic_OOP_Internal.hpp"

/// @file
/// @brief Implements phased construction and validation of the BASIC OOP index.

namespace il::frontends::basic::detail {

//===----------------------------------------------------------------------===//
// OopIndexBuilder Implementation
//===----------------------------------------------------------------------===//

/// @brief Joins the currently active namespace path.
/// @details Precomputes the exact output size, then concatenates @ref nsStack_
///          with period separators without modifying the stack.
/// @return Dot-qualified namespace prefix, or an empty string at top level.
std::string OopIndexBuilder::joinNamespace() const {
    if (nsStack_.empty())
        return {};
    std::string prefix;
    std::size_t size = 0;
    for (const auto &s : nsStack_)
        size += s.size() + 1;
    if (size)
        size -= 1;
    prefix.reserve(size);
    for (std::size_t i = 0; i < nsStack_.size(); ++i) {
        if (i)
            prefix.push_back('.');
        prefix += nsStack_[i];
    }
    return prefix;
}

/// @brief Validates a property and registers its synthesized accessor methods.
/// @details Getter and setter visibility may not exceed property visibility.
///          Present accessors become @c get_ and @c set_ method records carrying
///          the property's static and type metadata. Static accessor bodies are
///          scanned for invalid ME expressions.
/// @param prop Parsed property declaration to inspect.
/// @param info Mutable class record receiving synthesized methods and locations.
/// @post Every present accessor has a corresponding entry in @p info even when
///       a validation diagnostic was emitted.
void OopIndexBuilder::processPropertyDecl(const PropertyDecl &prop, ClassInfo &info) {
    /// Maps the two access levels to an ordering used for permissiveness checks.
    auto rank = [](Access a) { return a == Access::Public ? 1 : 0; };

    // Validate accessor access levels
    if (prop.get.present && rank(prop.get.access) > rank(prop.access)) {
        if (emitter_) {
            emitter_->emit(il::support::Severity::Error,
                           "B2113",
                           prop.loc,
                           1,
                           "getter access cannot be more permissive than property access");
        }
    }
    if (prop.set.present && rank(prop.set.access) > rank(prop.access)) {
        if (emitter_) {
            emitter_->emit(il::support::Severity::Error,
                           "B2114",
                           prop.loc,
                           1,
                           "setter access cannot be more permissive than property access");
        }
    }

    // Synthesize getter
    if (prop.get.present) {
        ClassInfo::MethodInfo mi;
        mi.sig.access = prop.get.access;
        mi.sig.returnType = prop.type;
        mi.isStatic = prop.isStatic;
        mi.isPropertyAccessor = true;
        mi.isGetter = true;
        std::string mname = std::string("get_") + prop.name;
        info.methods[mname] = std::move(mi);
        info.methodLocs[mname] = prop.loc;

        if (prop.isStatic) {
            // Static accessors cannot use the implicit instance receiver.
            checkMeInStaticContext(
                prop.get.body, emitter_, "B2103", "'ME' is not allowed in static method");
        }
    }

    // Synthesize setter
    if (prop.set.present) {
        ClassInfo::MethodInfo mi;
        mi.sig.access = prop.set.access;
        mi.sig.paramTypes = {prop.type};
        mi.isStatic = prop.isStatic;
        mi.isPropertyAccessor = true;
        mi.isGetter = false;
        std::string mname = std::string("set_") + prop.name;
        info.methods[mname] = std::move(mi);
        info.methodLocs[mname] = prop.loc;

        if (prop.isStatic) {
            // Static accessors cannot use the implicit instance receiver.
            checkMeInStaticContext(
                prop.set.body, emitter_, "B2103", "'ME' is not allowed in static method");
        }
    }
}

/// @brief Records constructor metadata and validates static/instance rules.
/// @details Static constructors are unique, parameterless, and forbidden from
///          using ME. Instance constructors replace the stored constructor
///          parameter signature and are scanned for locals that shadow
///          instance fields.
/// @param ctor Constructor declaration to process.
/// @param info Mutable class metadata receiving constructor flags and signature.
/// @param classDecl Containing class used by shadowing diagnostics.
/// @param fieldNames Instance-field names borrowed for the body scan.
/// @post @p info records the constructor kind even when validation reports an
///       error.
void OopIndexBuilder::processConstructorDecl(const ConstructorDecl &ctor,
                                             ClassInfo &info,
                                             const ClassDecl &classDecl,
                                             const std::unordered_set<std::string> &fieldNames) {
    if (ctor.isStatic) {
        if (info.hasStaticCtor && emitter_) {
            emitter_->emit(il::support::Severity::Error,
                           "B2104",
                           ctor.loc,
                           1,
                           "multiple static constructors not allowed");
        }
        info.hasStaticCtor = true;

        if (!ctor.params.empty() && emitter_) {
            emitter_->emit(il::support::Severity::Error,
                           "B2105",
                           ctor.loc,
                           1,
                           "static constructor cannot have parameters");
        }

        // Static constructors cannot use the implicit instance receiver.
        checkMeInStaticContext(
            ctor.body, emitter_, "B2106", "'ME' is not allowed in static constructor");
    } else {
        info.hasConstructor = true;
        info.ctorParams.clear();
        info.ctorParams.reserve(ctor.params.size());
        for (const auto &param : ctor.params) {
            ClassInfo::CtorParam sigParam;
            sigParam.type = param.type;
            sigParam.isArray = param.is_array;
            info.ctorParams.push_back(sigParam);
        }
        checkMemberShadowing(ctor.body, classDecl, fieldNames, emitter_);
    }
}

/// @brief Converts one method declaration into indexed method metadata.
/// @details Collects parameter types, derives a return type from an explicit
///          annotation or identifier suffix, preserves an explicit object
///          return class, validates return coverage and field shadowing, and
///          records virtual/abstract/final flags with an initially unassigned
///          slot. Static bodies are scanned for ME.
/// @param method Method declaration to convert.
/// @param info Mutable class metadata receiving the method and source location.
/// @param classDecl Containing class used for diagnostics.
/// @param fieldNames Instance-field names borrowed for shadow checks.
/// @post @p info contains the method keyed by its parsed name; an existing
///       same-key record is replaced.
void OopIndexBuilder::processMethodDecl(const MethodDecl &method,
                                        ClassInfo &info,
                                        const ClassDecl &classDecl,
                                        const std::unordered_set<std::string> &fieldNames) {
    MethodSig sig;
    sig.paramTypes.reserve(method.params.size());
    for (const auto &param : method.params)
        sig.paramTypes.push_back(param.type);

    if (method.ret.has_value())
        sig.returnType = method.ret;
    else if (auto suffixType = inferAstTypeFromSuffix(method.name))
        sig.returnType = suffixType;

    sig.access = method.access;

    // BUG-099 fix: Store return class name for object-returning methods
    if (!method.explicitClassRetQname.empty()) {
        std::string qualifiedClassName;
        for (size_t i = 0; i < method.explicitClassRetQname.size(); ++i) {
            if (i > 0)
                qualifiedClassName += ".";
            qualifiedClassName += method.explicitClassRetQname[i];
        }
        sig.returnClassName = qualifiedClassName;
    }

    emitMissingReturn(classDecl, method, emitter_);
    checkMemberShadowing(method.body, classDecl, fieldNames, emitter_);

    ClassInfo::MethodInfo mi;
    mi.sig = std::move(sig);
    mi.isStatic = method.isStatic;
    mi.isVirtual = method.isVirtual || method.isOverride;
    mi.isAbstract = method.isAbstract;
    mi.isFinal = method.isFinal;
    mi.slot = -1;
    info.methods[method.name] = std::move(mi);
    info.methodLocs[method.name] = method.loc;

    if (method.isStatic) {
        // Static methods cannot use the implicit instance receiver.
        checkMeInStaticContext(
            method.body, emitter_, "B2103", "'ME' is not allowed in static method");
    }
}

/// @brief Diagnoses case-insensitive field/method name collisions.
/// @details Compares every indexed method with the supplied instance fields and
///          emits B2017 at the method location for the first matching field.
///          Property accessors participate through their synthesized names.
/// @param info Class metadata containing methods and their source locations.
/// @param classDecl Containing declaration supplying a fallback location.
/// @param fieldNames Instance-field names to compare case-insensitively.
/// @post Emits at most one collision diagnostic per indexed method.
void OopIndexBuilder::checkFieldMethodCollisions(
    ClassInfo &info,
    const ClassDecl &classDecl,
    const std::unordered_set<std::string> &fieldNames) {
    for (const auto &[methodName, methodInfo] : info.methods) {
        for (const auto &fieldName : fieldNames) {
            if (string_utils::iequals(methodName, fieldName)) {
                if (emitter_) {
                    auto locIt = info.methodLocs.find(methodName);
                    il::support::SourceLoc loc =
                        locIt != info.methodLocs.end() ? locIt->second : classDecl.loc;
                    std::string msg = "method '" + methodName + "' conflicts with field '" +
                                      fieldName + "' (names are case-insensitive); " +
                                      "rename one to avoid runtime errors";
                    emitter_->emit(il::support::Severity::Error,
                                   "B2017",
                                   loc,
                                   static_cast<uint32_t>(methodName.size()),
                                   std::move(msg));
                }
                break;
            }
        }
    }
}

/// @brief Builds and registers one class's declaration-level metadata.
/// @details Qualifies the class in the current namespace, records its unresolved
///          base, partitions static and instance fields, processes supported
///          members, synthesizes a default constructor when needed, checks
///          collisions, and retains raw implemented-interface names for later
///          resolution.
/// @param classDecl Parsed class declaration to index.
/// @post @ref index_ contains a class entry keyed by its qualified name.
/// @post Base and interface relationships remain unresolved until
///       @ref resolveBasesAndImplements.
void OopIndexBuilder::processClassDecl(const ClassDecl &classDecl) {
    ClassInfo info;
    info.name = classDecl.name;
    info.loc = classDecl.loc;
    info.isAbstract = classDecl.isAbstract;
    info.isFinal = classDecl.isFinal;

    std::string prefix = joinNamespace();
    if (!prefix.empty())
        info.qualifiedName = prefix + "." + classDecl.name;
    else
        info.qualifiedName = classDecl.name;

    if (classDecl.baseName)
        rawBases_.emplace(info.qualifiedName, std::make_pair(*classDecl.baseName, classDecl.loc));

    // Collect fields
    info.fields.reserve(classDecl.fields.size());
    std::unordered_set<std::string> classFieldNames;
    classFieldNames.reserve(classDecl.fields.size());

    for (const auto &field : classDecl.fields) {
        ClassInfo::FieldInfo fi{field.name,
                                field.type,
                                field.access,
                                field.isArray,
                                field.arrayExtents,
                                field.objectClassName};
        if (field.isStatic)
            info.staticFields.push_back(std::move(fi));
        else {
            info.fields.push_back(std::move(fi));
            classFieldNames.insert(field.name);
        }
    }

    // Process members
    for (const auto &member : classDecl.members) {
        if (!member)
            continue;

        switch (member->stmtKind()) {
            case Stmt::Kind::PropertyDecl:
                processPropertyDecl(static_cast<const PropertyDecl &>(*member), info);
                break;
            case Stmt::Kind::ConstructorDecl:
                processConstructorDecl(static_cast<const ConstructorDecl &>(*member),
                                       info,
                                       classDecl,
                                       classFieldNames);
                break;
            case Stmt::Kind::DestructorDecl: {
                info.hasDestructor = true;
                const auto &dtor = static_cast<const DestructorDecl &>(*member);
                checkMemberShadowing(dtor.body, classDecl, classFieldNames, emitter_);
                break;
            }
            case Stmt::Kind::MethodDecl:
                processMethodDecl(
                    static_cast<const MethodDecl &>(*member), info, classDecl, classFieldNames);
                break;
            default:
                break;
        }
    }

    if (!info.hasConstructor)
        info.hasSynthCtor = true;

    // BUG-106 fix: Check field/method collisions
    checkFieldMethodCollisions(info, classDecl, classFieldNames);

    // Capture raw implements list
    for (const auto &implQN : classDecl.implementsQualifiedNames) {
        std::string dotted = joinQualified(implQN);
        if (!dotted.empty())
            info.rawImplements.push_back(std::move(dotted));
    }

    index_.classes()[info.qualifiedName] = std::move(info);
}

/// @brief Recursively registers object-model declarations from a statement list.
/// @details Namespace declarations extend @ref nsStack_ only while their bodies
///          are scanned. Class, interface, and enum declarations dispatch to
///          their dedicated processors; unrelated statements are ignored.
/// @param stmts Statements in the current lexical namespace.
/// @post @ref nsStack_ has the same size and contents it had on entry.
void OopIndexBuilder::scanDeclarations(const std::vector<StmtPtr> &stmts) {
    for (const auto &stmtPtr : stmts) {
        if (!stmtPtr)
            continue;

        switch (stmtPtr->stmtKind()) {
            case Stmt::Kind::NamespaceDecl: {
                const auto &ns = static_cast<const NamespaceDecl &>(*stmtPtr);
                for (const auto &seg : ns.path)
                    nsStack_.push_back(seg);
                scanDeclarations(ns.body);
                nsStack_.resize(nsStack_.size() >= ns.path.size() ? nsStack_.size() - ns.path.size()
                                                                  : 0);
                break;
            }
            case Stmt::Kind::ClassDecl:
                processClassDecl(static_cast<const ClassDecl &>(*stmtPtr));
                break;
            case Stmt::Kind::InterfaceDecl:
                processInterfaceDecl(static_cast<const InterfaceDecl &>(*stmtPtr));
                break;
            case Stmt::Kind::EnumDecl:
                processEnumDecl(static_cast<const EnumDecl &>(*stmtPtr));
                break;
            default:
                break;
        }
    }
}

/// @brief Builds and registers one interface's ordered method slots.
/// @details Rejects properties and static methods, diagnoses duplicate method
///          names, derives suffix-based return types when necessary, and
///          assigns a fresh interface identifier before publication.
/// @param idecl Parsed interface declaration to index.
/// @post A non-empty qualified interface name is present in
///       @ref OopIndex::interfacesByQname.
/// @note Invalid members are diagnosed and omitted where required, while the
///       remaining interface is still indexed.
void OopIndexBuilder::processInterfaceDecl(const InterfaceDecl &idecl) {
    InterfaceInfo ii;
    ii.qualifiedName = joinQualified(idecl.qualifiedName);
    if (ii.qualifiedName.empty())
        return;

    ii.ifaceId = index_.allocateInterfaceId();

    std::unordered_set<std::string> seen;
    for (const auto &mem : idecl.members) {
        if (!mem)
            continue;

        if (auto *pd = as<const PropertyDecl>(*mem)) {
            if (emitter_) {
                emitter_->emit(il::support::Severity::Error,
                               "B2115",
                               pd->loc,
                               1,
                               "interfaces cannot declare properties (methods only)");
            }
            continue;
        }

        if (auto *md = as<const MethodDecl>(*mem)) {
            if (md->isStatic && emitter_) {
                emitter_->emit(il::support::Severity::Error,
                               "B2116",
                               md->loc,
                               1,
                               "interfaces cannot declare STATIC methods");
            }

            if (seen.contains(md->name)) {
                if (emitter_) {
                    std::string msg = "interface '" + ii.qualifiedName +
                                      "' declares duplicate method '" + md->name + "'.";
                    emitter_->emit(il::support::Severity::Error,
                                   "E_IFACE_DUP_METHOD",
                                   md->loc,
                                   static_cast<uint32_t>(md->name.size()),
                                   std::move(msg));
                }
                continue;
            }
            seen.insert(md->name);

            IfaceMethodSig slot;
            slot.name = md->name;
            for (const auto &p : md->params)
                slot.paramTypes.push_back(p.type);
            if (md->ret)
                slot.returnType = md->ret;
            else if (auto suffixType = inferAstTypeFromSuffix(md->name))
                slot.returnType = suffixType;
            ii.slots.push_back(std::move(slot));
        }
    }
    index_.interfacesByQname()[ii.qualifiedName] = std::move(ii);
}

/// @brief Collects top-level USING imports and aliases for base resolution.
/// @param stmts Statement list to inspect for direct @ref UsingDecl nodes.
/// @post Namespace imports are inserted into @ref UsingContext::imports and
///       aliases are stored by canonical alias name in
///       @ref UsingContext::aliases.
/// @note This function does not recurse into namespace bodies.
void OopIndexBuilder::collectUsingDirectives(const std::vector<StmtPtr> &stmts) {
    for (const auto &stmtPtr : stmts) {
        if (!stmtPtr || stmtPtr->stmtKind() != Stmt::Kind::UsingDecl)
            continue;

        const auto &u = static_cast<const UsingDecl &>(*stmtPtr);
        std::string nsPath = joinQualified(u.namespacePath);
        if (nsPath.empty())
            continue;

        if (!u.alias.empty())
            usingCtx_.aliases[CanonicalizeIdent(u.alias)] = nsPath;
        else
            usingCtx_.imports.insert(nsPath);
    }
}

/// @brief Expands the leading alias of a dotted qualified name.
/// @param q Candidate qualified name.
/// @return @p q unchanged when it has no period or its leading segment is not
///         a known alias; otherwise the alias target joined to the remaining
///         segments.
/// @note A bare alias without a trailing qualified segment is intentionally
///       not expanded by this helper.
std::string OopIndexBuilder::expandAlias(const std::string &q) const {
    auto pos = q.find('.');
    if (pos == std::string::npos)
        return q;

    std::string first = q.substr(0, pos);
    std::string firstCanon = CanonicalizeIdent(first);
    auto itAlias = usingCtx_.aliases.find(firstCanon);
    if (itAlias == usingCtx_.aliases.end())
        return q;

    std::string tail = q.substr(pos + 1);
    if (tail.empty())
        return itAlias->second;
    return itAlias->second + "." + tail;
}

/// @brief Resolves a base-class name without consulting USING imports.
/// @details Tries an already qualified name, a sibling in @p classQ's namespace,
///          and finally the raw top-level name, in that order.
/// @param classQ Qualified name of the derived class providing namespace context.
/// @param raw Raw or alias-expanded base spelling.
/// @return Qualified class key from @ref index_, or an empty string when no
///         candidate exists.
std::string OopIndexBuilder::resolveBase(const std::string &classQ, const std::string &raw) const {
    if (raw.empty())
        return {};

    // Already qualified?
    if (raw.find('.') != std::string::npos) {
        if (index_.classes().contains(raw))
            return raw;
    }

    // Try sibling in same namespace
    auto lastDot = classQ.rfind('.');
    std::string prefix = (lastDot == std::string::npos) ? std::string{} : classQ.substr(0, lastDot);
    std::string candidate = prefix.empty() ? raw : (prefix + "." + raw);
    if (index_.classes().contains(candidate))
        return candidate;

    // Fallback to raw as top-level
    if (index_.classes().contains(raw))
        return raw;

    return {};
}

/// @brief Resolves an implemented-interface name in class-relative scope.
/// @details Tries an already qualified name, a sibling in @p classQ's namespace,
///          and the raw top-level name. USING imports and aliases are not
///          applied on this path.
/// @param classQ Qualified implementing-class name supplying namespace context.
/// @param raw Raw interface spelling.
/// @return Qualified interface key from @ref index_, or an empty string when
///         unresolved.
std::string OopIndexBuilder::resolveInterface(const std::string &classQ,
                                              const std::string &raw) const {
    if (raw.find('.') != std::string::npos) {
        if (index_.interfacesByQname().contains(raw))
            return raw;
    }

    auto lastDot = classQ.rfind('.');
    std::string prefix = (lastDot == std::string::npos) ? std::string{} : classQ.substr(0, lastDot);
    std::string candidate = prefix.empty() ? raw : (prefix + "." + raw);
    if (index_.interfacesByQname().contains(candidate))
        return candidate;

    if (index_.interfacesByQname().contains(raw))
        return raw;

    return {};
}

/// @brief Resolves deferred base classes and implemented interfaces.
/// @details Expands leading aliases for base names, then tries lexical and
///          top-level resolution. Unqualified unresolved bases are additionally
///          searched across USING imports; one hit resolves, multiple hits
///          produce the catalogued ambiguity diagnostic, and no hit produces
///          B2101. Implemented interfaces are mapped to numeric interface IDs
///          when class-relative resolution succeeds.
/// @post Every class has a resolved or empty @ref ClassInfo::baseQualified and
///       a list of all successfully resolved interface IDs.
void OopIndexBuilder::resolveBasesAndImplements() {
    for (auto &entry : index_.classes()) {
        ClassInfo &ci = entry.second;
        auto it = rawBases_.find(ci.qualifiedName);

        if (it != rawBases_.end()) {
            const std::string &raw = it->second.first;
            std::string rawMaybeAliased = expandAlias(raw);
            std::string resolved = resolveBase(ci.qualifiedName, rawMaybeAliased);

            if (resolved.empty()) {
                // Try USING imports for unqualified names
                if (rawMaybeAliased.find('.') == std::string::npos) {
                    std::vector<std::string> hits;
                    for (const auto &imp : usingCtx_.imports) {
                        std::string cand = imp + "." + rawMaybeAliased;
                        if (index_.classes().contains(cand))
                            hits.push_back(std::move(cand));
                    }

                    if (hits.size() == 1)
                        resolved = std::move(hits.front());
                    else if (hits.size() > 1 && emitter_) {
                        il::frontends::basic::semutil::emitAmbiguousType(
                            *emitter_, it->second.second, 1, rawMaybeAliased, hits);
                    }
                }
            }

            if (resolved.empty() && emitter_) {
                std::string msg = std::string("base class not found: '") + raw + "'";
                emitter_->emit(
                    il::support::Severity::Error, "B2101", it->second.second, 1, std::move(msg));
            }

            ci.baseQualified = std::move(resolved);
        }

        // Resolve implemented interfaces
        for (const auto &raw : ci.rawImplements) {
            std::string resolved = resolveInterface(ci.qualifiedName, raw);
            if (!resolved.empty()) {
                const auto &iface = index_.interfacesByQname().at(resolved);
                ci.implementedInterfaces.push_back(iface.ifaceId);
            }
        }
    }
}

/// @brief Detects and cuts cycles in resolved class inheritance.
/// @details Performs a depth-first three-state traversal over qualified class
///          names. Encountering an edge to a visiting class emits B2102 and
///          clears the current class's base edge so later recursive phases
///          terminate safely.
/// @post No retained base chain contains a cycle encountered by this traversal.
void OopIndexBuilder::detectInheritanceCycles() {
    enum State : uint8_t {
        kUnvisited = 0,
        kVisiting = 1,
        kVisited = 2,
    };

    std::unordered_map<std::string, State> state;
    state.reserve(index_.classes().size());

    /// Recursively marks one class and follows its resolved base edge.
    std::function<void(const std::string &)> detectCycle;
    detectCycle = [&](const std::string &name) {
        auto it = state.find(name);
        if (it != state.end() && it->second != kUnvisited)
            return;

        state[name] = kVisiting;
        auto *cls = index_.findClass(name);
        if (cls && !cls->baseQualified.empty()) {
            auto st = state[cls->baseQualified];
            if (st == kVisiting) {
                if (emitter_) {
                    std::string msg = std::string("inheritance cycle involving '") + name + "'";
                    emitter_->emit(
                        il::support::Severity::Error, "B2102", cls->loc, 1, std::move(msg));
                }
                cls->baseQualified.clear();
            } else if (st == kUnvisited) {
                detectCycle(cls->baseQualified);
            }
        }
        state[name] = kVisited;
    };

    for (auto &entry : index_.classes())
        detectCycle(entry.first);
}

/// @brief Builds inherited virtual tables and validates override contracts.
/// @details Processes each base before its derived class, copies inherited slot
///          names, propagates unimplemented abstract members, then assigns new
///          slots or reuses inherited slots for virtual methods. Attempts to
///          override non-virtual or final methods and signature mismatches are
///          diagnosed while retaining deterministic layout metadata.
/// @post Every indexed class has a finalized @ref ClassInfo::vtable and each
///       indexed virtual method has its selected slot when one is available.
void OopIndexBuilder::buildVtables() {
    std::unordered_map<std::string, bool> processed;
    processed.reserve(index_.classes().size());

    /// Finds the nearest base-class declaration of a named method.
    auto findInBases =
        [&](const std::string &startClass,
            const std::string &methodName) -> std::pair<ClassInfo *, ClassInfo::MethodInfo *> {
        ClassInfo *cur = index_.findClass(startClass);
        while (cur && !cur->baseQualified.empty()) {
            ClassInfo *base = index_.findClass(cur->baseQualified);
            if (!base)
                break;
            auto mit = base->methods.find(methodName);
            if (mit != base->methods.end())
                return {base, &mit->second};
            cur = base;
        }
        return {nullptr, nullptr};
    };

    /// Recursively finalizes one class after ensuring its base is complete.
    std::function<void(const std::string &)> build;
    build = [&](const std::string &name) {
        if (processed[name])
            return;

        ClassInfo *ci = index_.findClass(name);
        if (!ci)
            return;

        // Ensure base is built first
        if (!ci->baseQualified.empty())
            build(ci->baseQualified);

        // Inherit base vtable
        std::vector<std::string> vtable;
        if (!ci->baseQualified.empty()) {
            ClassInfo *base = index_.findClass(ci->baseQualified);
            if (base) {
                vtable = base->vtable;
                for (const auto &mname : base->vtable) {
                    auto bit = base->methods.find(mname);
                    if (bit != base->methods.end()) {
                        const auto &bm = bit->second;
                        if (bm.isAbstract && ci->methods.find(mname) == ci->methods.end())
                            ci->isAbstract = true;
                    }
                }
            }
        }

        // Assign slots and validate overrides
        for (auto &mp : ci->methods) {
            const std::string &mname = mp.first;
            auto &mi = mp.second;

            if (!mi.isVirtual)
                continue;

            if (mi.isAbstract)
                ci->isAbstract = true;

            if (auto [base, bmi] = findInBases(name, mname); bmi != nullptr) {
                if (bmi->slot < 0) {
                    if (emitter_)
                        emitter_->emit(il::support::Severity::Error,
                                       "B2104",
                                       ci->methodLocs[mname],
                                       static_cast<uint32_t>(mname.size()),
                                       std::string("cannot override non-virtual '") + mname + "'");
                } else {
                    if (bmi->isFinal && emitter_) {
                        emitter_->emit(il::support::Severity::Error,
                                       "B2107",
                                       ci->methodLocs[mname],
                                       static_cast<uint32_t>(mname.size()),
                                       std::string("cannot override final '") + mname + "'");
                    }

                    const MethodSig &s1 = mi.sig;
                    const MethodSig &s2 = bmi->sig;
                    bool sigOk =
                        (s1.paramTypes == s2.paramTypes) && (s1.returnType == s2.returnType);
                    if (!sigOk && emitter_) {
                        emitter_->emit(il::support::Severity::Error,
                                       "B2103",
                                       ci->methodLocs[mname],
                                       static_cast<uint32_t>(mname.size()),
                                       std::string("override signature mismatch for '") + mname +
                                           "'");
                    }

                    mi.slot = bmi->slot;
                    if (mi.slot >= 0 && static_cast<std::size_t>(mi.slot) < vtable.size())
                        vtable[mi.slot] = mname;
                }
            } else {
                mi.slot = static_cast<int>(vtable.size());
                vtable.push_back(mname);
            }
        }

        ci->vtable = std::move(vtable);
        processed[name] = true;
    };

    for (auto &entry : index_.classes())
        build(entry.first);
}

/// @brief Verifies class methods against every resolved interface slot.
/// @details Searches each class and its bases for a name- and signature-matching
///          method. Missing or incompatible slots make the class abstract and
///          may emit E_CLASS_MISSES_IFACE_METHOD for a previously concrete
///          class. Each interface receives an ordered slot-to-method mapping;
///          missing slots remain empty.
/// @post @ref ClassInfo::ifaceSlotImpl contains one mapping for every resolved
///       interface ID found in the reverse lookup.
void OopIndexBuilder::checkInterfaceConformance() {
    /// Finds the first matching method in a class or its base chain.
    auto findMethodInClassOrBases = [&](const std::string &classQ,
                                        const std::string &name) -> const ClassInfo::MethodInfo * {
        const ClassInfo *cur = index_.findClass(classQ);
        if (!cur)
            return nullptr;
        if (auto it = cur->methods.find(name); it != cur->methods.end())
            return &it->second;
        while (cur && !cur->baseQualified.empty()) {
            cur = index_.findClass(cur->baseQualified);
            if (!cur)
                break;
            if (auto it2 = cur->methods.find(name); it2 != cur->methods.end())
                return &it2->second;
        }
        return nullptr;
    };

    /// Compares parameter lists and optional return types exactly.
    auto sigsMatch = [](const MethodSig &cls, const IfaceMethodSig &iface) {
        if (cls.paramTypes != iface.paramTypes)
            return false;
        if (cls.returnType.has_value() != iface.returnType.has_value())
            return false;
        if (cls.returnType && iface.returnType && *cls.returnType != *iface.returnType)
            return false;
        return true;
    };

    // Build reverse lookup: interface id -> InterfaceInfo
    std::unordered_map<int, const InterfaceInfo *> idToIface;
    for (const auto &p : index_.interfacesByQname())
        idToIface[p.second.ifaceId] = &p.second;

    for (auto &entry : index_.classes()) {
        ClassInfo &ci = entry.second;
        if (ci.implementedInterfaces.empty())
            continue;

        bool wasAbstract = ci.isAbstract;
        for (int ifaceId : ci.implementedInterfaces) {
            auto itF = idToIface.find(ifaceId);
            if (itF == idToIface.end())
                continue;

            const InterfaceInfo &iface = *itF->second;
            std::vector<std::string> mapping;
            mapping.resize(iface.slots.size());

            for (size_t slot = 0; slot < iface.slots.size(); ++slot) {
                const auto &slotSig = iface.slots[slot];
                const ClassInfo::MethodInfo *mi =
                    findMethodInClassOrBases(ci.qualifiedName, slotSig.name);

                if (!mi || !sigsMatch(mi->sig, slotSig)) {
                    ci.isAbstract = true;
                    if (!wasAbstract && emitter_) {
                        std::string msg = "class '" + ci.qualifiedName + "' does not implement '" +
                                          iface.qualifiedName + "." + slotSig.name + "'.";
                        emitter_->emit(il::support::Severity::Error,
                                       "E_CLASS_MISSES_IFACE_METHOD",
                                       ci.loc,
                                       static_cast<uint32_t>(ci.name.size()),
                                       std::move(msg));
                    }
                    continue;
                }
                mapping[slot] = slotSig.name;
            }
            ci.ifaceSlotImpl[ifaceId] = std::move(mapping);
        }
    }
}

/// @brief Rebuilds the complete OOP index from @p program.
/// @details Clears all previous index state and executes the ordered pipeline:
///          declaration scan, USING collection, relationship resolution, cycle
///          removal, virtual-table construction, and interface conformance.
/// @param program Parsed BASIC program whose top-level statements are scanned.
/// @post @ref index_ contains only metadata derived from @p program.
void OopIndexBuilder::build(const Program &program) {
    index_.clear();

    // Phase 1: scan classes, interfaces, and enums.
    scanDeclarations(program.main);

    // Phase 2: collect top-level USING directives for base resolution.
    collectUsingDirectives(program.main);

    // Phase 3: resolve bases and implemented interfaces.
    resolveBasesAndImplements();

    // Phase 4: detect and cut inheritance cycles.
    detectInheritanceCycles();

    // Phase 5: build virtual tables and validate overrides.
    buildVtables();

    // Phase 6: verify interface conformance and record slot mappings.
    checkInterfaceConformance();
}

/// @brief Builds and registers one enum's sequential member values.
/// @details Explicit values reset the running counter; subsequent implicit
///          values increment from that point. Duplicate exact member names emit
///          B2120 and are omitted.
/// @param enumDecl Parsed enum declaration to index.
/// @post @ref index_ contains an enum keyed by @p enumDecl's unqualified name.
void OopIndexBuilder::processEnumDecl(const EnumDecl &enumDecl) {
    OopIndex::EnumInfo info;
    info.name = enumDecl.name;

    std::unordered_set<std::string> seen;
    long long nextValue = 0;
    for (const auto &member : enumDecl.members) {
        if (!seen.insert(member.name).second) {
            if (emitter_) {
                emitter_->emit(il::support::Severity::Error,
                               "B2120",
                               enumDecl.loc,
                               1,
                               "duplicate enum member '" + member.name + "' in ENUM " +
                                   enumDecl.name);
            }
            continue;
        }

        OopIndex::EnumInfo::Member m;
        m.name = member.name;
        if (member.value.has_value())
            nextValue = member.value.value();
        m.value = nextValue;
        info.members.push_back(std::move(m));
        ++nextValue;
    }

    index_.enums().emplace(enumDecl.name, std::move(info));
}

} // namespace il::frontends::basic::detail
