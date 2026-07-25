//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/Lowerer_Procedure_Variables.cpp
// Purpose: Variable discovery, type inference, and storage resolution.
//
// Phase: Variable Collection (runs during metadata gathering)
//
// Key Invariants:
// - VarCollectWalker visits all expressions to discover symbol usage
// - Type inference prioritizes semantic analysis over name suffixes
// - Module-level object arrays are cached for cross-procedure access
// - Variable storage resolution considers local slots, module globals,
//   and implicit class fields
//
// Ownership/Lifetime: Operates on borrowed Lowerer instance.
//
//===----------------------------------------------------------------------===//

/**
 * @file Lowerer_Procedure_Variables.cpp
 * @brief Implements symbol discovery, slot typing, and variable storage lookup.
 *
 * AST traversal is read-only. Symbol/cache state belongs to Lowerer, emitted
 * addresses belong to the active function or runtime module-variable service,
 * and returned storage descriptors carry only borrowed IL values.
 */

#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/AstWalker.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/LoweringPipeline.hpp"
#include "frontends/basic/ProcedureSymbolTracker.hpp"
#include "frontends/basic/SemanticAnalyzer.hpp"
#include "frontends/basic/StringUtils.hpp"
#include "frontends/basic/TypeSuffix.hpp"
#include "il/runtime/RuntimeClassNames.hpp"

using namespace il::core;

namespace il::frontends::basic {

using pipeline_detail::coreTypeForAstType;

// =============================================================================
// Variable Collection Walker
// =============================================================================

namespace {

/// @brief AST walker that records symbol usage within a procedure body.
/// @details Traverses expressions and statements to discover variable references
///          prior to lowering.  Each visit marks the appropriate symbol as
///          referenced and, when necessary, records array-ness so the lowering
///          stage can allocate the correct slot types.  The walker never mutates
///          the AST; it solely updates the owning @ref Lowerer state.
///
///          Uses ProcedureSymbolTracker to centralize symbol tracking logic,
///          avoiding duplication with RuntimeNeedsScanner.
class VarCollectWalker final : public BasicAstWalker<VarCollectWalker> {
  public:
    /// @brief Create a walker bound to the current lowering instance.
    /// @param lowerer Borrowed lowering driver whose symbol tables are updated.
    explicit VarCollectWalker(Lowerer &lowerer) noexcept : lowerer_(lowerer), tracker_(lowerer) {}

    /// @brief Record usage of a scalar variable expression.
    /// @param expr Scalar reference whose name is tracked.
    void after(const VarExpr &expr) {
        tracker_.trackScalar(expr.name);
    }

    /// @brief Record usage of an array element expression.
    /// @param expr Array access whose base name is tracked.
    void after(const ArrayExpr &expr) {
        tracker_.trackArray(expr.name);
    }

    /// @brief Record usage of an array lower-bound expression.
    /// @param expr LBOUND query whose array name is tracked.
    void after(const LBoundExpr &expr) {
        tracker_.trackArray(expr.name);
    }

    /// @brief Record usage of an array upper-bound expression.
    /// @param expr UBOUND query whose array name is tracked.
    void after(const UBoundExpr &expr) {
        tracker_.trackArray(expr.name);
    }

    /// @brief Track variables introduced by DIM statements.
    /// @param stmt DIM declaration inspected before its children.
    void before(const DimStmt &stmt) {
        if (stmt.name.empty())
            return;
        if (!stmt.explicitClassQname.empty()) {
            std::string className =
                lowerer_.resolveQualifiedClassCasing(JoinDots(stmt.explicitClassQname));
            lowerer_.setSymbolObjectType(stmt.name, className);
        } else {
            lowerer_.setSymbolType(stmt.name, stmt.type);
        }
        lowerer_.markSymbolReferenced(stmt.name);
        if (stmt.isArray)
            lowerer_.markArray(stmt.name);
    }

    /// @brief Track constant declarations.
    /// @param stmt CONST declaration inspected before its initializer.
    void before(const ConstStmt &stmt) {
        if (stmt.name.empty())
            return;
        lowerer_.setSymbolType(stmt.name, stmt.type);
        lowerer_.markSymbolReferenced(stmt.name);
    }

    /// @brief Track STATIC variable declarations.
    /// @param stmt STATIC declaration inspected before its initializer.
    void before(const StaticStmt &stmt) {
        if (stmt.name.empty())
            return;
        lowerer_.setSymbolType(stmt.name, stmt.type);
        lowerer_.markSymbolReferenced(stmt.name);
        lowerer_.markStatic(stmt.name);
    }

    /// @brief Track variables re-dimensioned at runtime.
    /// @param stmt REDIM statement inspected before its bounds.
    void before(const ReDimStmt &stmt) {
        if (stmt.name.empty())
            return;
        lowerer_.markSymbolReferenced(stmt.name);
        lowerer_.markArray(stmt.name);
    }

    /// @brief Track optional catch variable introduced by TRY/CATCH.
    /// @param stmt TRY/CATCH node containing the optional binding.
    void before(const TryCatchStmt &stmt) {
        if (stmt.catchVar && !stmt.catchVar->empty()) {
            lowerer_.markSymbolReferenced(*stmt.catchVar);
        }
    }

    /// @brief Track resource variable introduced by USING.
    /// @param stmt USING node containing resource name and qualified type.
    void before(const UsingStmt &stmt) {
        if (!stmt.varName.empty()) {
            lowerer_.markSymbolReferenced(stmt.varName);

            // Build qualified class name from type parts
            std::string className;
            for (size_t i = 0; i < stmt.typeQualified.size(); ++i) {
                if (i > 0)
                    className += ".";
                className += stmt.typeQualified[i];
            }
            if (!className.empty()) {
                lowerer_.setSymbolObjectType(stmt.varName, className);
            }
        }
    }

    /// @brief Record loop induction variables referenced by FOR statements.
    /// @param stmt FOR node containing an optional variable expression.
    void before(const ForStmt &stmt) {
        if (stmt.varExpr) {
            if (auto *varExpr = as<VarExpr>(*stmt.varExpr)) {
                lowerer_.markSymbolReferenced(varExpr->name);
            }
        }
    }

    /// @brief Record loop induction variables referenced by NEXT statements.
    /// @param stmt NEXT node containing an optional variable name.
    void before(const NextStmt &stmt) {
        if (!stmt.var.empty())
            lowerer_.markSymbolReferenced(stmt.var);
    }

    /// @brief Record variables that participate in INPUT statements.
    /// @param stmt INPUT node containing scalar target names.
    void before(const InputStmt &stmt) {
        for (const auto &name : stmt.vars) {
            tracker_.trackScalar(name);
        }
    }

  private:
    /// Parent lowerer receiving declarations and type metadata.
    Lowerer &lowerer_;
    /// Shared symbol-tracking policy bound to @ref lowerer_.
    ProcedureSymbolTracker tracker_;
};

} // namespace

// =============================================================================
// Variable Discovery Entry Points
// =============================================================================

/// @brief Discover variable usage across a list of statements.
/// @details Drives @ref VarCollectWalker over each statement pointer, skipping
///          null entries to accommodate partially built AST lists.
/// @param stmts Statement pointers whose referenced symbols should be recorded.
void ProcedureLowering::collectVars(const std::vector<const Stmt *> &stmts) {
    VarCollectWalker walker(lowerer);
    for (const auto *stmt : stmts)
        if (stmt)
            walker.walkStmt(*stmt);
}

/// @brief Discover variable usage across an entire program.
/// @details Flattens procedure and main-body statements into a temporary array
///          before deferring to @ref collectVars(const std::vector<const Stmt *> &).
/// @param prog Program providing statements to analyse.
void ProcedureLowering::collectVars(const Program &prog) {
    std::vector<const Stmt *> ptrs;
    ptrs.reserve(prog.procs.size() + prog.main.size());
    for (const auto &s : prog.procs)
        ptrs.push_back(s.get());
    for (const auto &s : prog.main)
        ptrs.push_back(s.get());
    collectVars(ptrs);
}

/// @brief Forward variable discovery to the procedure lowering helper.
/// @param prog Program whose statements should be scanned for symbol usage.
void Lowerer::collectVars(const Program &prog) {
    procedureLowering->collectVars(prog);
}

/// @brief Forward variable discovery for an arbitrary statement list.
/// @param stmts Statements whose referenced symbols should be recorded.
void Lowerer::collectVars(const std::vector<const Stmt *> &stmts) {
    procedureLowering->collectVars(stmts);
}

// =============================================================================
// Type Inference
// =============================================================================

/// @brief Infer variable type from semantic analyzer, then suffix, then fallback.
/// @details BUG-001 FIX: Queries semantic analyzer for value-based type inference
///          before falling back to suffix-based naming convention.
/// @param lowerer Lowerer instance providing access to semantic analyzer.
/// @param name Variable name to query.
/// @return Best-effort type derived from semantic analysis or naming convention.
Type inferVariableTypeForLowering(const Lowerer &lowerer, std::string_view name) {
    // Query semantic analyzer for value-based type inference
    if (const auto *sema = lowerer.semanticAnalyzer()) {
        if (auto semaType = sema->lookupVarType(std::string{name})) {
            using SemaType = SemanticAnalyzer::Type;
            switch (*semaType) {
                case SemaType::Int:
                    return Type::I64;
                case SemaType::Float:
                    return Type::F64;
                case SemaType::String:
                    return Type::Str;
                case SemaType::Bool:
                    return Type::Bool;
                case SemaType::ArrayInt:
                    return Type::I64;
                case SemaType::ArrayString:
                    return Type::Str;
                case SemaType::ArrayObject:
                    return Type::I64;
                default:
                    break;
            }
        }
    }
    // Fall back to suffix-based inference
    return inferAstTypeFromName(name);
}

/// @brief Compute the lowering slot characteristics for a symbol.
/// @details Combines declared type information, inferred suffix defaults, and
///          object/array flags to produce the IL type stored in the procedure
///          frame together with helper booleans used for boolean packing and
///          array metadata allocation.
/// @param name Identifier describing the symbol.
/// @return Populated slot descriptor containing IL type and auxiliary flags.
Lowerer::SlotType Lowerer::getSlotType(std::string_view name) const {
    SlotType info;
    AstType astTy = inferVariableTypeForLowering(*this, name);
    /// Recognize the generic Object spelling without case sensitivity.
    auto isGenericObject = [](std::string_view cls) {
        return string_utils::iequals(cls, "object");
    };
    /// Recognize runtime String class spellings that use `str` storage.
    auto isRuntimeStringObject = [](std::string_view cls) {
        return string_utils::iequals(cls, il::runtime::RTCLASS_STRING) ||
               string_utils::iequals(cls, "Zanna.System.String");
    };

    const auto *sym = findSymbol(name);

    if (sym && !sym->isArray && sym->isObject && !sym->objectClass.empty() &&
        isRuntimeStringObject(sym->objectClass)) {
        info.type = Type(Type::Kind::Str);
        info.isArray = false;
        info.isBoolean = false;
        info.isObject = false;
        info.objectClass = sym->objectClass;
        return info;
    }

    if (sym && !sym->isArray && sym->isObject && !sym->objectClass.empty() &&
        !isGenericObject(sym->objectClass)) {
        info.type = Type(Type::Kind::Ptr);
        info.isArray = false;
        info.isBoolean = false;
        info.isObject = true;
        info.objectClass = sym->objectClass;
        return info;
    }

    // BUG-BAS-002 fix: Skip module-level object cache for procedure parameters.
    // Parameters should use their declared type, not be shadowed by module-level
    // object variables with the same name.
    if (!isProcParam(name)) {
        auto modObjIt = moduleObjectClass_.find(name);
        if (modObjIt != moduleObjectClass_.end() && !modObjIt->second.empty() &&
            !isGenericObject(modObjIt->second)) {
            if (isRuntimeStringObject(modObjIt->second)) {
                info.type = Type(Type::Kind::Str);
                info.isArray = false;
                info.isBoolean = false;
                info.isObject = false;
                info.objectClass = modObjIt->second;
                return info;
            }
            info.type = Type(Type::Kind::Ptr);
            info.isArray = false;
            info.isBoolean = false;
            info.isObject = true;
            info.objectClass = modObjIt->second;
            return info;
        }
    }

    if (sym) {
        // BUG-BAS-002 fix: Only treat as object if it has a valid object class.
        // This prevents module-level object variables from polluting constructor parameters.
        if (!sym->isArray && sym->isObject && !sym->objectClass.empty()) {
            if (isRuntimeStringObject(sym->objectClass)) {
                info.type = Type(Type::Kind::Str);
                info.isArray = false;
                info.isBoolean = false;
                info.isObject = false;
                info.objectClass = sym->objectClass;
                return info;
            }
            info.type = Type(Type::Kind::Ptr);
            info.isArray = false;
            info.isBoolean = false;
            info.isObject = true;
            info.objectClass = sym->objectClass;
            return info;
        }
        // Override with sym->type when:
        // 1. Semantic analysis has no type (BUG-019), OR
        // 2. This is a procedure parameter (BUG-BAS-002) - params use their declared type,
        //    not module-level semantic analysis type
        bool hasSemaType = false;
        if (semanticAnalyzer_) {
            hasSemaType = semanticAnalyzer_->lookupVarType(std::string{name}).has_value();
        }
        if (sym->hasType && (!hasSemaType || isProcParam(name)))
            astTy = sym->type;
        info.isArray = sym->isArray;
        if (info.isArray) {
            info.isObject = sym->isObject;
            info.objectClass = sym->objectClass;
        }
        if (sym->isBoolean && !info.isArray)
            info.isBoolean = true;
        else if (!sym->hasType && !info.isArray)
            info.isBoolean = (astTy == AstType::Bool);
        else
            info.isBoolean = false;
    } else {
        info.isArray = false;
        info.isBoolean = (astTy == AstType::Bool);
    }
    if (info.isArray)
        info.type = Type(Type::Kind::Ptr);
    else
        info.type = coreTypeForAstType(info.isBoolean ? AstType::Bool : astTy);
    return info;
}

// =============================================================================
// Variable Storage Resolution
// =============================================================================

/// @brief Resolve storage location for a variable by name.
/// @details Checks STATIC runtime storage first. A materialized local is used
///          unless cross-procedure/module rules route it to runtime-backed
///          module storage; an implicit active-class field is the final
///          fallback. Empty and unresolved names return no descriptor.
/// @param name Variable identifier to resolve.
/// @param loc Source location for error reporting.
/// @return Storage descriptor or nullopt if unresolved.
std::optional<Lowerer::VariableStorage> Lowerer::resolveVariableStorage(
    std::string_view name, il::support::SourceLoc loc) {
    if (name.empty())
        return std::nullopt;

    SlotType slotInfo = getSlotType(name);

    // STATIC variables use procedure-qualified runtime storage. (BUG-010)
    if (const auto *info = findSymbol(name)) {
        if (info->isStatic) {
            return resolveStaticVariableStorage(name, slotInfo);
        }
    }

    // Local/parameter symbols shadow module globals. (BUG-103/BUG-OOP-036)
    if (const auto *info = findSymbol(name)) {
        if (info->slotId) {
            bool isMain = (context().function() && context().function()->name == "main");
            bool isCrossProc = isCrossProcGlobal(std::string(name));

            // In SUB/FUNCTION, local variables always shadow module-level symbols
            if (!isMain && !isCrossProc)
                return VariableStorage{slotInfo, Value::temp(*info->slotId), false};

            // In @main, check if this is a cross-procedure global
            bool isModLevel =
                semanticAnalyzer_ && semanticAnalyzer_->isModuleLevelSymbol(std::string(name));
            isCrossProc = isModLevel && isCrossProc;

            if (!isCrossProc)
                return VariableStorage{slotInfo, Value::temp(*info->slotId), false};
        }
    }

    // Module-level globals use runtime-managed storage for cross-procedure sharing
    if (semanticAnalyzer_ && semanticAnalyzer_->isModuleLevelSymbol(std::string(name))) {
        bool isMain = (context().function() && context().function()->name == "main");
        if (!isMain || isCrossProcGlobal(std::string(name))) {
            return resolveModuleLevelStorage(name, slotInfo);
        }
    }

    // Try implicit class field access
    if (auto field = resolveImplicitField(name, loc)) {
        VariableStorage storage;
        storage.slotInfo = slotInfo;
        storage.slotInfo.type = field->ilType;
        storage.slotInfo.isArray = false;
        storage.slotInfo.isBoolean = (field->astType == AstType::Bool);
        storage.slotInfo.isObject = false;
        storage.slotInfo.objectClass.clear();
        storage.pointer = field->ptr;
        storage.isField = true;
        return storage;
    }

    return std::nullopt;
}

/// @brief Resolve storage for a STATIC variable.
/// @details STATIC variables are procedure-local persistent variables using
///          the rt_modvar infrastructure with procedure-qualified names.
/// @param name Variable identifier.
/// @param slotInfo Slot type information.
/// @return Storage descriptor pointing to runtime-managed address.
std::optional<Lowerer::VariableStorage> Lowerer::resolveStaticVariableStorage(
    std::string_view name, const SlotType &slotInfo) {
    // Construct scoped name: "ProcedureName.VariableName"
    std::string scopedName;
    if (auto *func = context().function()) {
        scopedName = std::string(func->name) + "." + std::string(name);
    } else {
        scopedName = std::string(name);
    }

    // Choose runtime helper based on IL type
    std::string callee = selectModvarAddrHelper(slotInfo.type.kind);

    // Emit scoped name constant and runtime call
    std::string label = getStringLabel(scopedName);
    Value nameStr = emitConstStr(label);
    Value addr = emitCallRet(Type(Type::Kind::Ptr), callee, {nameStr});

    VariableStorage storage;
    storage.slotInfo = slotInfo;
    storage.pointer = addr;
    storage.isField = false;
    return storage;
}

/// @brief Resolve storage for a module-level global variable.
/// @details Module globals use runtime storage for cross-procedure sharing.
/// @param name Variable identifier.
/// @param slotInfo Slot type information.
/// @return Storage descriptor pointing to runtime-managed address.
std::optional<Lowerer::VariableStorage> Lowerer::resolveModuleLevelStorage(
    std::string_view name, const SlotType &slotInfo) {
    std::string callee = selectModvarAddrHelper(slotInfo.type.kind);

    std::string label = getStringLabel(std::string(name));
    Value nameStr = emitConstStr(label);
    Value addr = emitCallRet(Type(Type::Kind::Ptr), callee, {nameStr});

    VariableStorage storage;
    storage.slotInfo = slotInfo;
    storage.pointer = addr;
    storage.isField = false;
    return storage;
}

/// @brief Select the appropriate rt_modvar_addr_* helper based on type kind.
/// @param kind IL type kind for the variable.
/// @return Runtime function name for address lookup.
std::string Lowerer::selectModvarAddrHelper(Type::Kind kind) {
    switch (kind) {
        case Type::Kind::I1:
            requireModvarAddrI1();
            return "rt_modvar_addr_i1";
        case Type::Kind::F64:
            requireModvarAddrF64();
            return "rt_modvar_addr_f64";
        case Type::Kind::Str:
            requireModvarAddrStr();
            return "rt_modvar_addr_str";
        case Type::Kind::Ptr:
            requireModvarAddrPtr();
            return "rt_modvar_addr_ptr";
        default:
            requireModvarAddrI64();
            return "rt_modvar_addr_i64";
    }
}

// =============================================================================
// Class Name Resolution
// =============================================================================

/// @brief Resolve canonical class name to declared qualified casing using OOP index.
/// @param qname Case-insensitive qualified class name (segments separated by '.').
/// @return Qualified name with original casing when found; otherwise @p qname.
std::string Lowerer::resolveQualifiedClassCasing(const std::string &qname) const {
    // Fast path: exact match
    if (const ClassInfo *ci = oopIndex_.findClass(qname))
        return ci->qualifiedName.empty() ? qname : ci->qualifiedName;
    // Case-insensitive match over indexed classes
    /// Create a lowercase copy for fallback class-name comparison.
    auto lower = [](const std::string &s) {
        std::string out;
        out.reserve(s.size());
        for (unsigned char c : s)
            out.push_back(static_cast<char>(std::tolower(c)));
        return out;
    };
    const std::string needle = lower(qname);
    for (const auto &p : oopIndex_.classes()) {
        const ClassInfo &ci = p.second;
        if (lower(ci.qualifiedName) == needle)
            return ci.qualifiedName;
    }
    return qname;
}

/// @brief Compute canonical layout key for class lookup.
/// @details Extracts the unqualified leaf name from a potentially qualified
///          class name after resolving casing via the OOP index.
/// @param className Class name to canonicalize.
/// @return Unqualified class name suitable for classLayouts_ lookup.
std::string Lowerer::canonicalLayoutKey(std::string_view className) const {
    std::string qname = resolveQualifiedClassCasing(std::string(className));
    auto lastDot = qname.find_last_of('.');
    std::string leaf = (lastDot == std::string::npos) ? qname : qname.substr(lastDot + 1);
    return leaf;
}

/// @brief Find class layout by name with fallback strategies.
/// @details Tries direct lookup, canonicalized key, and case-insensitive match.
/// @param className Class name to look up.
/// @return Pointer to layout or nullptr if not found.
const Lowerer::ClassLayout *Lowerer::findClassLayout(std::string_view className) const {
    // Try direct key (heterogeneous lookup)
    auto it = classLayouts_.find(className);
    if (it != classLayouts_.end())
        return &it->second;
    // Try canonicalized key
    std::string key = canonicalLayoutKey(className);
    auto it2 = classLayouts_.find(key);
    if (it2 != classLayouts_.end())
        return &it2->second;
    // Case-insensitive fallback
    /// Lowercase an owned key for case-insensitive layout fallback.
    auto lower = [](std::string s) {
        for (auto &c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::string needle = lower(key);
    for (const auto &p : classLayouts_) {
        std::string leaf = p.first;
        if (lower(leaf) == needle)
            return &p.second;
    }
    return nullptr;
}

// =============================================================================
// Module Object Array Caching
// =============================================================================

/// @brief Cache module-level object arrays from AST for cross-procedure access.
/// @details Scans main body DIM statements to populate element class caches.
/// @param main Main body statements to scan.
void Lowerer::cacheModuleObjectArraysFromAST(const std::vector<StmtPtr> &main) {
    moduleObjArrayElemClass_.clear();
    moduleObjectClass_.clear();
    moduleStrArrayNames_.clear();

    for (const auto &stmtPtr : main) {
        if (!stmtPtr)
            continue;

        if (stmtPtr->stmtKind() == Stmt::Kind::Dim) {
            const auto *dim = as<const DimStmt>(*stmtPtr);
            if (!dim)
                continue;

            // Cache string arrays
            if (dim->isArray && dim->type == AstType::Str) {
                moduleStrArrayNames_.insert(dim->name);
            }

            if (!dim->explicitClassQname.empty()) {
                std::string className;
                for (size_t i = 0; i < dim->explicitClassQname.size(); ++i) {
                    if (i > 0)
                        className += '.';
                    className += dim->explicitClassQname[i];
                }
                std::string resolvedClassName = resolveQualifiedClassCasing(className);
                if (dim->isArray)
                    moduleObjArrayElemClass_[dim->name] = resolvedClassName;
                else
                    moduleObjectClass_[dim->name] = resolvedClassName;
            }
        }
    }
}

/// @brief Cache module-level object arrays from symbol table.
/// @details Alternative to AST-based caching when symbols are already populated.
void Lowerer::cacheModuleObjectArraysFromSymbols() {
    moduleObjArrayElemClass_.clear();
    moduleStrArrayNames_.clear();
    for (const auto &p : symbols) {
        const std::string &name = p.first;
        const SymbolInfo &info = p.second;
        if (info.isArray && info.isObject && !info.objectClass.empty()) {
            moduleObjArrayElemClass_[name] = info.objectClass;
        }
        if (info.isArray && info.type == AstType::Str) {
            moduleStrArrayNames_.insert(name);
        }
    }
}

/// @brief Look up the element class for a module-level object array.
/// @param name Array name to query.
/// @return Class name or empty string if not an object array.
std::string Lowerer::lookupModuleArrayElemClass(std::string_view name) const {
    auto it = moduleObjArrayElemClass_.find(std::string{name});
    if (it == moduleObjArrayElemClass_.end())
        return {};
    return it->second;
}

/// @brief Check if a module-level variable is a string array.
/// @param name Variable name to check.
/// @return True if the name refers to a module-level string array.
bool Lowerer::isModuleStrArray(std::string_view name) const {
    return moduleStrArrayNames_.contains(std::string{name});
}

} // namespace il::frontends::basic
