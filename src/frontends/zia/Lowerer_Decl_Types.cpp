//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/zia/Lowerer_Decl_Types.cpp
// Purpose: Register Zia aggregate layouts and emit runtime type/interface
//          initialization.
// Key invariants:
//   - Aggregate layouts are registered before bodies are lowered.
//   - Base classes are registered before derived classes.
//   - Large class-registration sequences are partitioned into ordered helpers
//     so native backends never receive one unbounded initializer function.
//   - Interface tables are populated only after their classes and interfaces
//     have been registered.
// Ownership/Lifetime:
//   - Lowered metadata is owned by the Lowerer and destination IL module.
//   - AST declarations and semantic types are borrowed for the lowering pass.
// Cross-platform touchpoints:
//   - Generated IL is target-neutral; pointer-sized tables use the canonical
//     machine-word layout expected by every native backend.
// Links: src/frontends/zia/Lowerer.hpp,
//        src/codegen/aarch64/LoweringContext.hpp,
//        src/tests/zia/test_zia_iface_dispatch.cpp
//
//===----------------------------------------------------------------------===//

#include "frontends/zia/Lowerer.hpp"
#include "frontends/zia/RuntimeNames.hpp"

#include <algorithm>
#include <functional>
#include <unordered_set>
#include <utility>

namespace il::frontends::zia {

using namespace runtime;

//=============================================================================
// Type Layout Pre-Registration (BUG-FE-006 fix)
//=============================================================================

/// @brief Pre-register layouts for every class/struct/interface in a declaration list.
/// @param declarations Top-level (or namespace-scoped) declarations to scan.
/// @details Runs before bodies are lowered so types may reference one another regardless of
///          source order (BUG-FE-006). Recurses into namespaces, threading the namespace
///          prefix so nested types register under their qualified names.
void Lowerer::registerAllTypeLayouts(std::vector<DeclPtr> &declarations) {
    for (auto &decl : declarations) {
        if (decl->kind == DeclKind::Class) {
            registerClassLayout(*static_cast<ClassDecl *>(decl.get()));
        } else if (decl->kind == DeclKind::Struct) {
            registerStructLayout(*static_cast<StructDecl *>(decl.get()));
        } else if (decl->kind == DeclKind::Interface) {
            registerInterfaceLayout(*static_cast<InterfaceDecl *>(decl.get()));
        } else if (decl->kind == DeclKind::Namespace) {
            auto *ns = static_cast<NamespaceDecl *>(decl.get());
            std::string savedPrefix = namespacePrefix_;
            if (namespacePrefix_.empty())
                namespacePrefix_ = ns->name;
            else
                namespacePrefix_ = namespacePrefix_ + "." + ns->name;

            registerAllTypeLayouts(ns->declarations);

            namespacePrefix_ = savedPrefix;
        }
    }
}

/// @brief Register the field layout, class id, vtable, and interface set for a class.
/// @param decl Class declaration AST node.
/// @details Idempotent (skips if already registered) and skips uninstantiated generics. The
///          base class is registered first so its members can be inherited; this class then
///          computes its own field offsets and vtable on top of the inherited layout.
void Lowerer::registerClassLayout(ClassDecl &decl) {
    // Skip uninstantiated generic types
    if (!decl.genericParams.empty())
        return;

    std::string qualifiedName = declarationName(decl, decl.name);

    // Skip if already registered
    if (classTypes_.find(qualifiedName) != classTypes_.end())
        return;

    ClassTypeInfo info;
    info.name = qualifiedName;
    if (!decl.baseClass.empty()) {
        TypeRef baseType = sema_.resolveNamedType(decl.baseClass, decl.loc);
        info.baseClass = baseType ? baseType->name : decl.baseClass;
        if (ClassDecl *baseDecl = sema_.lookupClassDeclForType(info.baseClass))
            registerClassLayout(*baseDecl);
    }
    info.totalSize = kClassFieldsOffset;
    info.classId = nextClassId_++;
    info.vtableName = "__vtable_" + qualifiedName;

    for (const auto &iface : decl.interfaces) {
        TypeRef ifaceType = sema_.resolveNamedType(iface, decl.loc);
        info.implementedInterfaces.insert(ifaceType ? ifaceType->name : iface);
    }

    // Copy inherited fields from parent class
    if (!info.baseClass.empty()) {
        auto parentIt = classTypes_.find(info.baseClass);
        if (parentIt != classTypes_.end()) {
            inheritClassMembers(info, parentIt->second);
        }
    }

    // Compute field layout and build vtable from this class's own members
    computeClassFieldLayout(decl, info, qualifiedName);
    buildClassVtable(decl, info, qualifiedName);

    for (const auto &member : decl.members) {
        if (member->kind == DeclKind::Destructor) {
            info.hasUserDeinit = true;
            break;
        }
    }

    classTypes_[qualifiedName] = std::move(info);
}

/// @brief Register an interface's methods and their itable slot indices.
/// @param decl Interface declaration AST node.
/// @details Idempotent. Assigns each interface method a stable slot index keyed by
///          methodSlotKey() so implementing types can populate their itables consistently.
void Lowerer::registerInterfaceLayout(InterfaceDecl &decl) {
    std::string qualifiedName = declarationName(decl, decl.name);
    if (interfaceTypes_.find(qualifiedName) != interfaceTypes_.end())
        return;

    InterfaceTypeInfo info;
    info.name = qualifiedName;
    info.ifaceId = nextIfaceId_++;

    size_t slotIdx = 0;
    for (auto &member : decl.members) {
        if (member->kind != DeclKind::Method)
            continue;
        auto *method = static_cast<MethodDecl *>(member.get());
        info.methodMap[method->name] = method;
        info.methods.push_back(method);
        info.slotIndex[sema_.methodSlotKey(qualifiedName, method)] = slotIdx++;
    }

    interfaceTypes_[qualifiedName] = std::move(info);
}

/// @brief Assign instance-field offsets and sizes for a class's own (non-inherited) fields.
/// @param decl Class declaration providing the field members.
/// @param info Class type info to extend (offsets continue after any inherited fields).
/// @param qualifiedName Qualified class name used to look up semantic field types.
/// @details Static fields are skipped (they become module-level globals). Each field is
///          aligned and sized via the semantic inline layout so nested structs, tuples, and
///          fixed-size arrays occupy their full storage; weak fields are pointer-sized.
void Lowerer::computeClassFieldLayout(ClassDecl &decl,
                                      ClassTypeInfo &info,
                                      const std::string &qualifiedName) {
    (void)qualifiedName; // used implicitly via caller context
    for (auto &member : decl.members) {
        if (member->kind != DeclKind::Field)
            continue;

        auto *field = static_cast<FieldDecl *>(member.get());

        // Static fields become module-level globals, not instance fields
        if (field->isStatic)
            continue;

        TypeRef fieldType = sema_.getFieldType(qualifiedName, field->name);
        if (!fieldType && qualifiedName != decl.name)
            fieldType = sema_.getFieldType(decl.name, field->name);
        if (!fieldType)
            fieldType = field->type ? sema_.resolveType(field->type.get()) : types::unknown();

        // Compute size and alignment using semantic inline layout so nested
        // structs, tuples, and fixed-size arrays occupy their full storage.
        size_t fieldLayoutSize =
            field->isWeak ? getILTypeSize(Type(Type::Kind::Ptr)) : getSemanticTypeSize(fieldType);
        size_t fieldLayoutAlignment = field->isWeak ? getILTypeAlignment(Type(Type::Kind::Ptr))
                                                    : getSemanticTypeAlignment(fieldType);

        FieldLayout layout;
        layout.name = field->name;
        layout.type = fieldType;
        layout.isWeak = field->isWeak;
        layout.offset = alignTo(info.totalSize, fieldLayoutAlignment);
        layout.size = fieldLayoutSize;

        info.fieldIndex[field->name] = info.fields.size();
        info.fields.push_back(layout);
        info.totalSize = layout.offset + layout.size;
    }
}

/// @brief Populate the method map and vtable slot list from a class's own members.
/// @param decl Class declaration providing method and property members.
/// @param info Class type info whose vtable/method map are extended.
/// @param qualifiedName Qualified class name used to mangle method names and slot keys.
/// @details Non-static methods get (or override) a vtable slot keyed by methodSlotKey();
///          static methods are recorded in the method map but excluded from the vtable.
///          Properties are noted as `get_`/`set_` getter/setter names for later synthesis.
void Lowerer::buildClassVtable(ClassDecl &decl,
                               ClassTypeInfo &info,
                               const std::string &qualifiedName) {
    for (auto &member : decl.members) {
        if (member->kind == DeclKind::Method) {
            auto *method = static_cast<MethodDecl *>(member.get());
            info.methodMap[method->name] = method;
            info.methods.push_back(method);

            // Build vtable (static methods don't go in vtable)
            if (!method->isStatic) {
                std::string slotKey = sema_.methodSlotKey(qualifiedName, method);
                std::string methodQualName = sema_.loweredMethodName(qualifiedName, method);
                if (methodQualName.empty())
                    methodQualName = qualifiedName + "." + method->name;
                auto vtableIt = info.vtableIndex.find(slotKey);
                if (vtableIt != info.vtableIndex.end()) {
                    info.vtable[vtableIt->second] = methodQualName;
                } else {
                    info.vtableIndex[slotKey] = info.vtable.size();
                    info.vtable.push_back(methodQualName);
                }
            }
        } else if (member->kind == DeclKind::Property) {
            // Properties are synthesized into get_X/set_X methods during lowering
            auto *prop = static_cast<PropertyDecl *>(member.get());

            // Register getter as a method
            std::string getterName = "get_" + prop->name;
            info.propertyGetters.insert(getterName);

            // Register setter if present
            if (prop->setterBody) {
                std::string setterName = "set_" + prop->name;
                info.propertySetters.insert(setterName);
            }
        }
    }
}

/// @brief Copy a parent class's fields, vtable, and size into a derived class's layout.
/// @param info Derived class type info to seed (called before its own members are added).
/// @param parent Already-registered base class type info.
/// @details Inherited fields keep their parent offsets and the derived total size starts at
///          the parent's size, so the derived class appends new fields after the base layout.
void Lowerer::inheritClassMembers(ClassTypeInfo &info, const ClassTypeInfo &parent) {
    for (const auto &parentField : parent.fields) {
        info.fieldIndex[parentField.name] = info.fields.size();
        info.fields.push_back(parentField);
    }
    info.totalSize = parent.totalSize;
    info.vtable = parent.vtable;
    info.vtableIndex = parent.vtableIndex;
}

/// @brief Register the field layout and methods for a value type (`struct`).
/// @param decl Struct declaration AST node.
/// @details Idempotent and skips uninstantiated generics. Field offsets start at 0 (structs
///          have no object header); each field is aligned/sized via the semantic inline
///          layout, with weak fields treated as pointers. Methods are recorded for lowering.
void Lowerer::registerStructLayout(StructDecl &decl) {
    // Skip uninstantiated generic types
    if (!decl.genericParams.empty())
        return;

    std::string qualifiedName = declarationName(decl, decl.name);

    // Skip if already registered
    if (structTypes_.find(qualifiedName) != structTypes_.end())
        return;

    StructTypeInfo info;
    info.name = qualifiedName;
    info.totalSize = 0;
    info.classId = nextClassId_++;
    for (const auto &iface : decl.interfaces) {
        TypeRef ifaceType = sema_.resolveNamedType(iface, decl.loc);
        info.implementedInterfaces.insert(ifaceType ? ifaceType->name : iface);
    }

    for (auto &member : decl.members) {
        if (member->kind == DeclKind::Field) {
            auto *field = static_cast<FieldDecl *>(member.get());
            TypeRef fieldType = sema_.getFieldType(qualifiedName, field->name);
            if (!fieldType && qualifiedName != decl.name)
                fieldType = sema_.getFieldType(decl.name, field->name);
            if (!fieldType)
                fieldType = field->type ? sema_.resolveType(field->type.get()) : types::unknown();

            // Compute size and alignment using semantic inline layout so nested
            // structs, tuples, and fixed-size arrays occupy their full storage.
            size_t fieldLayoutSize = field->isWeak ? getILTypeSize(Type(Type::Kind::Ptr))
                                                   : getSemanticTypeSize(fieldType);
            size_t fieldLayoutAlignment = field->isWeak ? getILTypeAlignment(Type(Type::Kind::Ptr))
                                                        : getSemanticTypeAlignment(fieldType);

            FieldLayout layout;
            layout.name = field->name;
            layout.type = fieldType;
            layout.isWeak = field->isWeak;
            layout.offset = alignTo(info.totalSize, fieldLayoutAlignment);
            layout.size = fieldLayoutSize;

            info.fieldIndex[field->name] = info.fields.size();
            info.fields.push_back(layout);
            info.totalSize = layout.offset + layout.size;
        } else if (member->kind == DeclKind::Method) {
            auto *method = static_cast<MethodDecl *>(member.get());
            info.methodMap[method->name] = method;
            info.methods.push_back(method);
        }
    }

    structTypes_[qualifiedName] = std::move(info);
}

/// @brief No-op placeholder retained for a possible future vtable-pointer dispatch scheme.
/// @details Virtual dispatch is currently class_id-based rather than vtable-pointer based
///          (BUG-VL-011); the vtable layout is consumed at compile time by emitItableInit()
///          and dispatch lowering, so no per-class vtable global is emitted here.
void Lowerer::emitVtable(const ClassTypeInfo & /*info*/) {
    // BUG-VL-011: Virtual dispatch is now handled via class_id-based dispatch
    // instead of vtable pointers. The vtable info is used at compile time
    // to generate dispatch code, not runtime vtable lookup.
    // This function is kept as a placeholder for future vtable-based dispatch.
}

//=============================================================================
// Interface Registration and ITable Binding
//=============================================================================

namespace {
/// @brief Maximum estimated IL-result cost emitted into one class-registration function.
/// @details Native backends use function-local virtual-register namespaces. Keeping generated
///          registration helpers bounded prevents large applications from exhausting those
///          namespaces while retaining direct initialization for ordinary-sized modules.
constexpr size_t kClassRegistrationBatchCost = 2048;

/// @brief Compose the cache key identifying a struct→interface→method adapter.
/// @param structName Qualified name of the implementing value type.
/// @param ifaceName Qualified name of the target interface.
/// @param methodName Interface method name represented by the adapter.
/// @return Stable delimiter-separated cache key for the adapter tuple.
std::string itableAdapterKey(const std::string &structName,
                             const std::string &ifaceName,
                             const std::string &methodName) {
    return structName + "|" + ifaceName + "|" + methodName;
}
} // namespace

/// @brief ADR 0315: emit `__zia_layout_init` — every class's strong object slots.
/// @details For each class with a positive runtime id, walks its fields
///          (inherited first), skips weak fields, flattens inline aggregates
///          with collectManagedSlots, keeps object-kind slots (strings and weak
///          handles are never cycle members) and emits one
///          rt_obj_class_layout_begin plus one rt_obj_class_layout_add_slot per
///          slot. Classes without a strong slot register nothing, so their
///          instances are never tracked. Large modules batch the registrations
///          into bounded helpers exactly like the itable init. The function is
///          always emitted; the entry prologue calls it unconditionally.
void Lowerer::emitClassLayoutInit() {
    Function *savedFunc = currentFunc_;
    auto savedLocals = std::exchange(locals_, {});
    auto savedSlots = std::exchange(slots_, {});
    auto savedLocalTypes = std::exchange(localTypes_, {});

    struct ClassSlots {
        int64_t classId;
        std::vector<ManagedSlot> slots;
    };

    std::vector<ClassSlots> registrations;
    std::vector<std::string> classNames;
    for (const auto &entry : classTypes_)
        classNames.push_back(entry.first);
    std::sort(classNames.begin(), classNames.end());
    for (const auto &className : classNames) {
        const ClassTypeInfo &info = classTypes_.at(className);
        if (info.classId <= 0)
            continue;
        std::vector<ManagedSlot> slots;
        for (const auto &field : info.fields) {
            if (field.isWeak)
                continue;
            std::vector<ManagedSlot> fieldSlots;
            collectManagedSlots(field.type, field.offset, fieldSlots);
            for (const auto &slot : fieldSlots) {
                if (slot.kind == 1)
                    slots.push_back(slot);
            }
        }
        if (slots.empty())
            continue;
        registrations.push_back({static_cast<int64_t>(info.classId), std::move(slots)});
    }

    auto startInitFunction = [&](const std::string &name) {
        auto &initFn = builder_->startFunction(name, Type(Type::Kind::Void), {});
        initFn.moduleInitializer = false;
        currentFunc_ = &initFn;
        definedFunctions_.insert(name);
        blockMgr_.bind(builder_.get(), &initFn);
        builder_->createBlock(initFn, "entry_0", {});
        setBlock(initFn.blocks.size() - 1);
    };

    auto emitRegistration = [&](const ClassSlots &reg) {
        emitCall(runtime::kRtObjClassLayoutBegin,
                 {Value::constInt(reg.classId),
                  Value::constInt(static_cast<int64_t>(reg.slots.size()))});
        for (const auto &slot : reg.slots) {
            emitCall(runtime::kRtObjClassLayoutAddSlot,
                     {Value::constInt(reg.classId),
                      Value::constInt(static_cast<int64_t>(slot.offset)),
                      Value::constInt(slot.kind)});
        }
    };

    std::vector<std::string> helpers;
    size_t totalCost = 0;
    for (const auto &reg : registrations)
        totalCost += 1 + reg.slots.size();
    if (totalCost > kClassRegistrationBatchCost) {
        size_t batchBegin = 0;
        size_t batchCost = 0;
        size_t helperIndex = 0;
        auto emitBatch = [&](size_t begin, size_t end) {
            const std::string helperName = "__zia_layout_init_" + std::to_string(helperIndex++);
            helpers.push_back(helperName);
            startInitFunction(helperName);
            for (size_t i = begin; i < end; ++i)
                emitRegistration(registrations[i]);
            emitRetVoid();
        };
        for (size_t i = 0; i < registrations.size(); ++i) {
            const size_t cost = 1 + registrations[i].slots.size();
            if (i > batchBegin && batchCost + cost > kClassRegistrationBatchCost) {
                emitBatch(batchBegin, i);
                batchBegin = i;
                batchCost = 0;
            }
            batchCost += cost;
        }
        if (batchBegin < registrations.size())
            emitBatch(batchBegin, registrations.size());
    }

    startInitFunction(runtime::kZiaLayoutInit);
    if (helpers.empty()) {
        for (const auto &reg : registrations)
            emitRegistration(reg);
    } else {
        for (const auto &helperName : helpers)
            emitCall(helperName, {});
    }
    emitRetVoid();

    currentFunc_ = savedFunc;
    locals_ = std::move(savedLocals);
    slots_ = std::move(savedSlots);
    localTypes_ = std::move(savedLocalTypes);
    if (currentFunc_)
        blockMgr_.bind(builder_.get(), currentFunc_);
}

/// @brief Emit the `__zia_iface_init` startup function that wires up runtime type metadata.
/// @details Called once from the program entry point when interfaces exist. In four phases it
///          (1) builds struct→interface adapters, (2) registers each class and struct with the
///          runtime (vtable + base-class id) in base-before-derived order, registers each
///          interface, (3) binds class itables by resolving each interface slot to the
///          implementing (or default) method, and (4) binds struct itables through the
///          adapters. The caller's function/local context is saved and restored around it.
void Lowerer::emitItableInit() {
    // Skip if no interfaces are defined (no call was emitted in start())
    if (interfaceTypes_.empty())
        return;

    // Save current function context
    Function *savedFunc = currentFunc_;
    auto savedLocals = std::exchange(locals_, {});
    auto savedSlots = std::exchange(slots_, {});
    auto savedLocalTypes = std::exchange(localTypes_, {});

    std::unordered_map<std::string, std::string> structIfaceAdapters;

    for (const auto &[structName, structInfo] : structTypes_) {
        for (const auto &ifaceName : structInfo.implementedInterfaces) {
            auto ifaceIt = interfaceTypes_.find(ifaceName);
            if (ifaceIt == interfaceTypes_.end())
                continue;
            const InterfaceTypeInfo &ifaceInfo = ifaceIt->second;
            for (auto *ifaceMethod : ifaceInfo.methods) {
                MethodDecl *implMethod = structInfo.findMethod(ifaceMethod->name);
                if (!implMethod)
                    continue;
                std::string key = itableAdapterKey(structName, ifaceName, ifaceMethod->name);
                structIfaceAdapters[key] =
                    emitStructInterfaceAdapter(structInfo, ifaceInfo, ifaceMethod, implMethod);
            }
        }
    }

    // Phase 1: Register each class before interface bindings reference it.
    std::vector<std::string> classOrder;
    std::unordered_set<std::string> visitedClasses;
    /// @brief Appends a class after recursively visiting its base class.
    /// @param className Registered class name.
    std::function<void(const std::string &)> visitClass = [&](const std::string &className) {
        if (visitedClasses.count(className))
            return;
        auto classIt = classTypes_.find(className);
        if (classIt == classTypes_.end())
            return;
        if (!classIt->second.baseClass.empty())
            visitClass(classIt->second.baseClass);
        visitedClasses.insert(className);
        classOrder.push_back(className);
    };
    for (const auto &entry : classTypes_)
        visitClass(entry.first);

    /// @brief Starts an empty, parameterless initialization function and binds the IL builder.
    /// @param name Compiler-reserved function name.
    /// @param moduleInitializer Whether the function is an automatic module initializer.
    auto startInitFunction = [&](const std::string &name, bool moduleInitializer) {
        auto &initFn = builder_->startFunction(name, Type(Type::Kind::Void), {});
        initFn.moduleInitializer = moduleInitializer;
        currentFunc_ = &initFn;
        definedFunctions_.insert(name);
        blockMgr_.bind(builder_.get(), &initFn);
        builder_->createBlock(initFn, "entry_0", {});
        setBlock(initFn.blocks.size() - 1);
    };

    /// @brief Emits one class's vtable allocation/population and runtime registration.
    /// @param className Qualified registered class name.
    auto emitClassRegistration = [&](const std::string &className) {
        auto classIt = classTypes_.find(className);
        if (classIt == classTypes_.end())
            return;
        const ClassTypeInfo &classInfo = classIt->second;

        const size_t slotCount = classInfo.vtable.size();
        const int64_t bytes = slotCount > 0 ? static_cast<int64_t>(slotCount * 8ULL) : 8LL;
        Value vtablePtr = emitCallRet(Type(Type::Kind::Ptr), "rt_alloc", {Value::constInt(bytes)});

        for (size_t s = 0; s < slotCount; ++s) {
            int64_t offset = static_cast<int64_t>(s * 8ULL);
            Value slotPtr =
                emitBinary(Opcode::GEP, Type(Type::Kind::Ptr), vtablePtr, Value::constInt(offset));
            const std::string &methodName = classInfo.vtable[s];
            if (methodName.empty()) {
                emitStore(slotPtr, Value::null(), Type(Type::Kind::Ptr));
            } else {
                emitStore(slotPtr, Value::global(methodName), Type(Type::Kind::Ptr));
            }
        }

        int64_t baseClassId = -1;
        if (!classInfo.baseClass.empty()) {
            auto baseIt = classTypes_.find(classInfo.baseClass);
            if (baseIt != classTypes_.end())
                baseClassId = static_cast<int64_t>(baseIt->second.classId);
        }

        Value qnameStr = emitConstStr(stringTable_.intern(classInfo.name));
        emitCall("rt_register_class_with_base_rs",
                 {Value::constInt(static_cast<int64_t>(classInfo.classId)),
                  vtablePtr,
                  qnameStr,
                  Value::constInt(static_cast<int64_t>(slotCount)),
                  Value::constInt(baseClassId)});
    };

    size_t totalClassRegistrationCost = 0;
    for (const auto &className : classOrder) {
        auto classIt = classTypes_.find(className);
        if (classIt != classTypes_.end())
            totalClassRegistrationCost += 2 + classIt->second.vtable.size();
    }

    std::vector<std::string> classInitHelpers;
    if (totalClassRegistrationCost > kClassRegistrationBatchCost) {
        size_t batchBegin = 0;
        size_t batchCost = 0;
        size_t helperIndex = 0;

        /// @brief Emits one bounded helper for the half-open class-order range.
        /// @param begin First class index to register.
        /// @param end One-past-last class index to register.
        auto emitClassBatch = [&](size_t begin, size_t end) {
            const std::string helperName = "__zia_class_init_" + std::to_string(helperIndex++);
            classInitHelpers.push_back(helperName);
            startInitFunction(helperName, false);
            for (size_t i = begin; i < end; ++i)
                emitClassRegistration(classOrder[i]);
            emitRetVoid();
        };

        for (size_t i = 0; i < classOrder.size(); ++i) {
            auto classIt = classTypes_.find(classOrder[i]);
            const size_t classCost =
                classIt == classTypes_.end() ? 0 : 2 + classIt->second.vtable.size();
            if (i > batchBegin && batchCost + classCost > kClassRegistrationBatchCost) {
                emitClassBatch(batchBegin, i);
                batchBegin = i;
                batchCost = 0;
            }
            batchCost += classCost;
        }
        if (batchBegin < classOrder.size())
            emitClassBatch(batchBegin, classOrder.size());
    }

    // The public initializer remains the single ordered entry point. Small
    // modules retain direct registration to keep their IL compact; large ones
    // call the bounded helpers emitted above.
    startInitFunction("__zia_iface_init", true);
    if (classInitHelpers.empty()) {
        for (const auto &className : classOrder)
            emitClassRegistration(className);
    } else {
        for (const auto &helperName : classInitHelpers)
            emitCall(helperName, {});
    }

    for (const auto &[structName, structInfo] : structTypes_) {
        if (structInfo.implementedInterfaces.empty())
            continue;

        Value vtablePtr = emitCallRet(Type(Type::Kind::Ptr),
                                      "rt_alloc",
                                      {Value::constInt(static_cast<long long>(kMachineWordSize))});
        Value qnameStr = emitConstStr(stringTable_.intern(structName));
        emitCall("rt_register_class_with_base_rs",
                 {Value::constInt(static_cast<int64_t>(structInfo.classId)),
                  vtablePtr,
                  qnameStr,
                  Value::constInt(0),
                  Value::constInt(-1)});
    }

    // Phase 2: Register each interface
    for (const auto &[ifaceName, ifaceInfo] : interfaceTypes_) {
        // rt_register_interface_direct_rs(ifaceId, qname, slotCount)
        Value qnameStr = emitConstStr(stringTable_.intern(ifaceName));
        emitCall("rt_register_interface_direct_rs",
                 {Value::constInt(static_cast<int64_t>(ifaceInfo.ifaceId)),
                  qnameStr,
                  Value::constInt(static_cast<int64_t>(ifaceInfo.methods.size()))});
    }

    // Phase 3: For each class implementing an interface, build and bind itable
    for (const auto &[entityName, entityInfo] : classTypes_) {
        for (const auto &ifaceName : entityInfo.implementedInterfaces) {
            auto ifaceIt = interfaceTypes_.find(ifaceName);
            if (ifaceIt == interfaceTypes_.end())
                continue;
            const InterfaceTypeInfo &ifaceInfo = ifaceIt->second;
            if (ifaceInfo.methods.empty())
                continue;

            // Allocate itable: slotCount * 8 bytes
            size_t slotCount = ifaceInfo.methods.size();
            int64_t bytes = static_cast<int64_t>(slotCount * 8ULL);
            Value itablePtr =
                emitCallRet(Type(Type::Kind::Ptr), "rt_alloc", {Value::constInt(bytes)});

            // Populate each slot with a function pointer
            for (size_t s = 0; s < slotCount; ++s) {
                const std::string slotKey =
                    sema_.methodSlotKey(ifaceInfo.name, ifaceInfo.methods[s]);
                int64_t offset = static_cast<int64_t>(s * 8ULL);
                Value slotPtr = emitBinary(
                    Opcode::GEP, Type(Type::Kind::Ptr), itablePtr, Value::constInt(offset));

                // Find the implementing method in the class (or its bases)
                std::string implName;
                std::string searchEntity = entityName;
                while (!searchEntity.empty()) {
                    auto entIt = classTypes_.find(searchEntity);
                    if (entIt == classTypes_.end())
                        break;
                    auto vtIt = entIt->second.vtableIndex.find(slotKey);
                    if (vtIt != entIt->second.vtableIndex.end()) {
                        implName = entIt->second.vtable[vtIt->second];
                        break;
                    }
                    searchEntity = entIt->second.baseClass;
                }

                if (implName.empty()) {
                    implName = defaultInterfaceMethodName(ifaceInfo, ifaceInfo.methods[s]);
                }

                if (implName.empty()) {
                    emitStore(slotPtr, Value::null(), Type(Type::Kind::Ptr));
                } else {
                    emitStore(slotPtr, Value::global(implName), Type(Type::Kind::Ptr));
                }
            }

            // Bind the itable: rt_bind_interface(typeId, ifaceId, itable)
            emitCall("rt_bind_interface",
                     {Value::constInt(static_cast<int64_t>(entityInfo.classId)),
                      Value::constInt(static_cast<int64_t>(ifaceInfo.ifaceId)),
                      itablePtr});
        }
    }

    // Phase 4: Struct interface bindings use heap wrappers plus small adapters
    // that translate wrapper self pointers back to the inline struct payload.
    for (const auto &[structName, structInfo] : structTypes_) {
        for (const auto &ifaceName : structInfo.implementedInterfaces) {
            auto ifaceIt = interfaceTypes_.find(ifaceName);
            if (ifaceIt == interfaceTypes_.end())
                continue;
            const InterfaceTypeInfo &ifaceInfo = ifaceIt->second;
            if (ifaceInfo.methods.empty())
                continue;

            size_t slotCount = ifaceInfo.methods.size();
            int64_t bytes = static_cast<int64_t>(slotCount * 8ULL);
            Value itablePtr =
                emitCallRet(Type(Type::Kind::Ptr), "rt_alloc", {Value::constInt(bytes)});

            for (size_t s = 0; s < slotCount; ++s) {
                MethodDecl *ifaceMethod = ifaceInfo.methods[s];
                int64_t offset = static_cast<int64_t>(s * 8ULL);
                Value slotPtr = emitBinary(
                    Opcode::GEP, Type(Type::Kind::Ptr), itablePtr, Value::constInt(offset));

                std::string key = itableAdapterKey(structName, ifaceName, ifaceMethod->name);
                auto adapterIt = structIfaceAdapters.find(key);
                if (adapterIt == structIfaceAdapters.end()) {
                    std::string defaultImpl = defaultInterfaceMethodName(ifaceInfo, ifaceMethod);
                    if (defaultImpl.empty())
                        emitStore(slotPtr, Value::null(), Type(Type::Kind::Ptr));
                    else
                        emitStore(slotPtr, Value::global(defaultImpl), Type(Type::Kind::Ptr));
                } else {
                    emitStore(slotPtr, Value::global(adapterIt->second), Type(Type::Kind::Ptr));
                }
            }

            emitCall("rt_bind_interface",
                     {Value::constInt(static_cast<int64_t>(structInfo.classId)),
                      Value::constInt(static_cast<int64_t>(ifaceInfo.ifaceId)),
                      itablePtr});
        }
    }

    emitRetVoid();

    // Restore previous function context
    currentFunc_ = savedFunc;
    locals_ = std::move(savedLocals);
    slots_ = std::move(savedSlots);
    localTypes_ = std::move(savedLocalTypes);
}

/// @brief Return the lowered name of an interface method's default implementation.
/// @param ifaceInfo Interface that owns the method.
/// @param ifaceMethod Interface method to resolve.
/// @return The lowered default-impl function name, or "" when the method is abstract
///         (no body) and therefore has no default to fall back to.
std::string Lowerer::defaultInterfaceMethodName(const InterfaceTypeInfo &ifaceInfo,
                                                MethodDecl *ifaceMethod) {
    if (!ifaceMethod || !ifaceMethod->body)
        return "";
    std::string lowered = sema_.loweredMethodName(ifaceInfo.name, ifaceMethod);
    if (!lowered.empty())
        return lowered;
    return ifaceInfo.name + "." + ifaceMethod->name;
}

/// @brief Emit (and cache) a thunk that lets a value type satisfy an interface slot.
/// @param structInfo The implementing value type.
/// @param ifaceInfo The interface being satisfied.
/// @param ifaceMethod The interface method signature the slot expects.
/// @param implMethod The struct method that implements it.
/// @return The lowered adapter function name (reused if already emitted).
/// @details Because interface dispatch passes a heap-wrapper `self`, the adapter offsets the
///          wrapper pointer by kClassFieldsOffset to reach the inline struct payload, then
///          forwards all arguments to the real struct method and returns its result.
std::string Lowerer::emitStructInterfaceAdapter(const StructTypeInfo &structInfo,
                                                const InterfaceTypeInfo &ifaceInfo,
                                                MethodDecl *ifaceMethod,
                                                MethodDecl *implMethod) {
    std::string adapterName =
        structInfo.name + ".__iface_" + ifaceInfo.name + "." + ifaceMethod->name;
    if (definedFunctions_.count(adapterName))
        return adapterName;

    TypeRef methodType = sema_.getMethodType(ifaceInfo.name, ifaceMethod);
    if (!methodType)
        methodType = sema_.getMethodType(structInfo.name, implMethod);

    std::vector<TypeRef> paramTypes;
    TypeRef returnType = types::voidType();
    if (methodType && methodType->kind == TypeKindSem::Function) {
        paramTypes = methodType->paramTypes();
        returnType = methodType->returnType();
    }

    Type ilReturnType = mapType(returnType);
    std::vector<il::core::Param> params;
    params.push_back({"self", Type(Type::Kind::Ptr)});
    for (size_t i = 0; i < ifaceMethod->params.size(); ++i) {
        TypeRef paramType = i < paramTypes.size() ? paramTypes[i] : types::unknown();
        params.push_back({ifaceMethod->params[i].name, mapType(paramType)});
    }

    currentFunc_ = &builder_->startFunction(adapterName, ilReturnType, params);
    definedFunctions_.insert(adapterName);
    blockMgr_.bind(builder_.get(), currentFunc_);
    locals_.clear();
    slots_.clear();
    localTypes_.clear();
    deferredTemps_.clear();
    currentStructType_ = nullptr;
    currentClassType_ = nullptr;

    builder_->createBlock(*currentFunc_, "entry_0", currentFunc_->params);
    size_t entryIdx = currentFunc_->blocks.size() - 1;
    setBlock(entryIdx);

    const auto &bp = currentFunc_->blocks[entryIdx].params;
    Value wrapperSelf = Value::temp(bp[0].id);
    Value structSelf = emitGEP(wrapperSelf, static_cast<int64_t>(kClassFieldsOffset));

    std::vector<Value> args;
    args.reserve(bp.size());
    args.push_back(structSelf);
    for (size_t i = 1; i < bp.size(); ++i)
        args.push_back(Value::temp(bp[i].id));

    std::string implName = sema_.loweredMethodName(structInfo.name, implMethod);
    if (implName.empty())
        implName = structInfo.name + "." + implMethod->name;

    if (ilReturnType.kind == Type::Kind::Void) {
        emitCall(implName, args);
        releaseDeferredTemps();
        emitRetVoid();
    } else {
        Value result = emitCallRet(ilReturnType, implName, args);
        consumeDeferred(result);
        releaseDeferredTemps();
        emitRet(result);
    }

    currentFunc_ = nullptr;
    currentReturnType_ = nullptr;
    deferredTemps_.clear();
    return adapterName;
}

//=============================================================================
// On-Demand Generic Type Instantiation
//=============================================================================

/// @brief Look up a struct's type info, instantiating a generic struct on demand.
/// @param typeName Qualified or unqualified struct name.
/// @return Pointer to the cached/created StructTypeInfo, or nullptr if it cannot be resolved.
/// @details Returns a cached entry when present; an unqualified name is matched against a
///          unique registered suffix (ambiguous matches yield nullptr). Otherwise, if the
///          name is an instantiated generic, builds its layout from the template with
///          substituted field types and queues method lowering via
///          @c pendingStructInstantiations_.
const StructTypeInfo *Lowerer::getOrCreateStructTypeInfo(const std::string &typeName) {
    // Check existing cache
    auto it = structTypes_.find(typeName);
    if (it != structTypes_.end()) {
        return &it->second;
    }

    if (typeName.find('.') == std::string::npos) {
        const StructTypeInfo *matched = nullptr;
        const std::string suffix = "." + typeName;
        for (const auto &[registeredName, info] : structTypes_) {
            if (registeredName.size() < suffix.size() ||
                registeredName.compare(
                    registeredName.size() - suffix.size(), suffix.size(), suffix) != 0)
                continue;
            if (matched)
                return nullptr;
            matched = &info;
        }
        if (matched)
            return matched;
    }

    // Check if this is an instantiated generic
    if (!sema_.isInstantiatedGeneric(typeName)) {
        return nullptr;
    }

    // Get the original generic declaration
    Decl *genericDecl = sema_.getGenericDeclForInstantiation(typeName);
    if (!genericDecl || genericDecl->kind != DeclKind::Struct) {
        return nullptr;
    }

    auto *structDecl = static_cast<StructDecl *>(genericDecl);

    // Build StructTypeInfo for the instantiated type
    StructTypeInfo info;
    info.name = typeName;
    info.totalSize = 0;
    info.classId = nextClassId_++;
    for (const auto &iface : structDecl->interfaces) {
        info.implementedInterfaces.insert(iface);
    }

    for (auto &member : structDecl->members) {
        if (member->kind == DeclKind::Field) {
            auto *field = static_cast<FieldDecl *>(member.get());
            // Get the substituted field type from Sema
            TypeRef fieldType = sema_.getFieldType(typeName, field->name);
            if (!fieldType)
                fieldType = types::unknown();

            // Compute size and alignment using semantic inline layout so nested
            // structs, tuples, and fixed-size arrays occupy their full storage.
            size_t fieldLayoutSize = field->isWeak ? getILTypeSize(Type(Type::Kind::Ptr))
                                                   : getSemanticTypeSize(fieldType);
            size_t fieldLayoutAlignment = field->isWeak ? getILTypeAlignment(Type(Type::Kind::Ptr))
                                                        : getSemanticTypeAlignment(fieldType);

            FieldLayout layout;
            layout.name = field->name;
            layout.type = fieldType;
            layout.isWeak = field->isWeak;
            layout.offset = alignTo(info.totalSize, fieldLayoutAlignment);
            layout.size = fieldLayoutSize;

            info.fieldIndex[field->name] = info.fields.size();
            info.fields.push_back(layout);
            info.totalSize = layout.offset + layout.size;
        } else if (member->kind == DeclKind::Method) {
            auto *method = static_cast<MethodDecl *>(member.get());
            info.methodMap[method->name] = method;
            info.methods.push_back(method);
        }
    }

    // Store the struct type info, capturing the slot to avoid re-hashing the key.
    StructTypeInfo &slot = (structTypes_[typeName] = std::move(info));

    // Defer method lowering until after all declarations are processed
    // (we may be in the middle of lowering another function body)
    pendingStructInstantiations_.push_back(typeName);

    return &slot;
}

/// @brief Look up a class's type info, instantiating a generic class on demand.
/// @param typeName Qualified or unqualified class name.
/// @return Pointer to the cached/created ClassTypeInfo, or nullptr if it cannot be resolved.
/// @details Mirrors getOrCreateStructTypeInfo() for reference types: cache hit, unique-suffix
///          match for unqualified names, else build an instantiated generic from its template
///          (inheriting base members and building the vtable) and queue method lowering via
///          @c pendingClassInstantiations_.
const ClassTypeInfo *Lowerer::getOrCreateClassTypeInfo(const std::string &typeName) {
    // Check existing cache
    auto it = classTypes_.find(typeName);
    if (it != classTypes_.end()) {
        return &it->second;
    }

    if (typeName.find('.') == std::string::npos) {
        const ClassTypeInfo *matched = nullptr;
        const std::string suffix = "." + typeName;
        for (const auto &[registeredName, info] : classTypes_) {
            if (registeredName.size() < suffix.size() ||
                registeredName.compare(
                    registeredName.size() - suffix.size(), suffix.size(), suffix) != 0)
                continue;
            if (matched)
                return nullptr;
            matched = &info;
        }
        if (matched)
            return matched;
    }

    // Check if this is an instantiated generic
    if (!sema_.isInstantiatedGeneric(typeName)) {
        return nullptr;
    }

    // Get the original generic declaration
    Decl *genericDecl = sema_.getGenericDeclForInstantiation(typeName);
    if (!genericDecl || genericDecl->kind != DeclKind::Class) {
        return nullptr;
    }

    auto *classDecl = static_cast<ClassDecl *>(genericDecl);

    // Build ClassTypeInfo for the instantiated type
    ClassTypeInfo info;
    info.name = typeName;
    info.baseClass = classDecl->baseClass;
    info.totalSize = kClassFieldsOffset; // Space for header + vtable ptr
    info.classId = nextClassId_++;
    info.vtableName = "__vtable_" + typeName;

    // Store implemented interfaces
    for (const auto &iface : classDecl->interfaces) {
        info.implementedInterfaces.insert(iface);
    }

    // Handle inheritance (if base class exists, copy its fields)
    if (!classDecl->baseClass.empty()) {
        auto parentIt = classTypes_.find(classDecl->baseClass);
        if (parentIt != classTypes_.end()) {
            inheritClassMembers(info, parentIt->second);
        }
    }

    // Process members
    for (auto &member : classDecl->members) {
        if (member->kind == DeclKind::Field) {
            auto *field = static_cast<FieldDecl *>(member.get());
            // Get the substituted field type from Sema
            TypeRef fieldType = sema_.getFieldType(typeName, field->name);
            if (!fieldType)
                fieldType = types::unknown();

            // Compute size and alignment using semantic inline layout so nested
            // structs, tuples, and fixed-size arrays occupy their full storage.
            size_t fieldLayoutSize = field->isWeak ? getILTypeSize(Type(Type::Kind::Ptr))
                                                   : getSemanticTypeSize(fieldType);
            size_t fieldLayoutAlignment = field->isWeak ? getILTypeAlignment(Type(Type::Kind::Ptr))
                                                        : getSemanticTypeAlignment(fieldType);

            FieldLayout layout;
            layout.name = field->name;
            layout.type = fieldType;
            layout.isWeak = field->isWeak;
            layout.offset = alignTo(info.totalSize, fieldLayoutAlignment);
            layout.size = fieldLayoutSize;

            info.fieldIndex[field->name] = info.fields.size();
            info.fields.push_back(layout);
            info.totalSize = layout.offset + layout.size;
        } else if (member->kind == DeclKind::Method) {
            auto *method = static_cast<MethodDecl *>(member.get());
            info.methodMap[method->name] = method;
            info.methods.push_back(method);

            // Build vtable
            std::string slotKey = sema_.methodSlotKey(typeName, method);
            std::string methodQualName = sema_.loweredMethodName(typeName, method);
            if (methodQualName.empty())
                methodQualName = typeName + "." + method->name;
            auto vtableIt = info.vtableIndex.find(slotKey);
            if (vtableIt != info.vtableIndex.end()) {
                info.vtable[vtableIt->second] = methodQualName;
            } else {
                info.vtableIndex[slotKey] = info.vtable.size();
                info.vtable.push_back(methodQualName);
            }
        }
    }

    // Store the class type info, capturing the slot to avoid re-hashing the key.
    ClassTypeInfo &slot = (classTypes_[typeName] = std::move(info));

    // Defer method lowering until after all declarations are processed
    // (we may be in the middle of lowering another function body)
    pendingClassInstantiations_.push_back(typeName);

    return &slot;
}

/// @brief Classify one field layout into the debugger's read strategy.
/// @param field Lowered class-field layout to export.
/// @return Debugger-facing name, type, offset, display metadata, and storage
///         classification for @p field.
/// @details Storage follows the IL type the field's stores/loads use; managed
///          vs raw pointers are split by semantic kind so the debugger never
///          dereferences a non-runtime pointer. Anything wider than a machine
///          word is an inline aggregate (fixed array/tuple storage) and is
///          exported opaque so it stays a typed leaf.
static DebugFieldExport exportField(const FieldLayout &field) {
    DebugFieldExport out;
    out.name = field.name;
    out.typeName = field.type ? field.type->toString() : "";
    out.offset = field.offset;

    if (field.size > kMachineWordSize) {
        out.store = DebugFieldStore::Opaque;
        return out;
    }
    if (field.isWeak) {
        out.store = DebugFieldStore::Weak;
        return out;
    }
    if (!field.type) {
        out.store = DebugFieldStore::Opaque;
        return out;
    }

    switch (toILType(*field.type)) {
        case il::core::Type::Kind::I64:
            out.store = DebugFieldStore::I64;
            out.boolDisplay = field.type->kind == TypeKindSem::Boolean;
            return out;
        case il::core::Type::Kind::I32:
            out.store = DebugFieldStore::I32;
            return out;
        case il::core::Type::Kind::I16:
            out.store = DebugFieldStore::I16;
            return out;
        case il::core::Type::Kind::I1:
            out.store = DebugFieldStore::I1;
            return out;
        case il::core::Type::Kind::F64:
            out.store = DebugFieldStore::F64;
            return out;
        case il::core::Type::Kind::Str:
            out.store = DebugFieldStore::Str;
            return out;
        case il::core::Type::Kind::Ptr:
            switch (field.type->kind) {
                case TypeKindSem::Class:
                case TypeKindSem::Interface:
                case TypeKindSem::List:
                case TypeKindSem::Map:
                case TypeKindSem::Set:
                case TypeKindSem::Optional:
                case TypeKindSem::Result:
                case TypeKindSem::Struct:
                case TypeKindSem::Function:
                case TypeKindSem::Tuple:
                case TypeKindSem::Any:
                    out.store = DebugFieldStore::Managed;
                    return out;
                default:
                    out.store = DebugFieldStore::Raw;
                    return out;
            }
        default:
            out.store = DebugFieldStore::Opaque;
            return out;
    }
}

/// @brief Export all instantiated class layouts for debugger object inspection.
/// @return Table keyed by runtime class identifier, with fields classified by
///         exportField().
/// @details Generic templates without an allocated runtime class identifier
///          are omitted because they cannot have live runtime instances.
DebugClassLayoutExport Lowerer::collectDebugClassLayouts() const {
    DebugClassLayoutExport table;
    for (const auto &[typeName, info] : classTypes_) {
        if (info.classId < 0)
            continue; // uninstantiated generic template: no runtime instances
        DebugClassExport entry;
        entry.qname = info.name.empty() ? typeName : info.name;
        entry.fields.reserve(info.fields.size());
        for (const auto &field : info.fields)
            entry.fields.push_back(exportField(field));
        table.emplace(static_cast<int64_t>(info.classId), std::move(entry));
    }
    return table;
}

} // namespace il::frontends::zia
