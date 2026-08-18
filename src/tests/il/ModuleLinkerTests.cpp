//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/il/ModuleLinkerTests.cpp
// Purpose: Test the IL module linker — export/import resolution, name collision
//          prefixing, extern merging, and init function injection.
// Key invariants: Linked modules must resolve all imports and have no duplicates.
// Ownership/Lifetime: Test-owned modules.
// Links: docs/adr/0003-il-linkage-and-module-linking.md
//
//===----------------------------------------------------------------------===//

#include "il/link/ModuleLinker.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Extern.hpp"
#include "il/core/Function.hpp"
#include "il/core/Global.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Linkage.hpp"
#include "il/core/Module.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/Value.hpp"

#include "tests/TestHarness.hpp"

#include <algorithm>
#include <string>

using namespace il::core;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Create a trivial function with a single entry block that returns void.
static Function makeVoidFunc(const std::string &name, Linkage linkage) {
    Function fn;
    fn.name = name;
    fn.retType = Type(Type::Kind::Void);
    fn.linkage = linkage;

    if (linkage != Linkage::Import) {
        BasicBlock entry;
        entry.label = "entry";
        Instr ret;
        ret.op = Opcode::Ret;
        entry.instructions.push_back(ret);
        fn.blocks.push_back(std::move(entry));
    }

    return fn;
}

/// Create a function that returns i64 with a trivial body.
static Function makeI64Func(const std::string &name, Linkage linkage, long long retVal = 0) {
    Function fn;
    fn.name = name;
    fn.retType = Type(Type::Kind::I64);
    fn.linkage = linkage;

    if (linkage != Linkage::Import) {
        BasicBlock entry;
        entry.label = "entry";
        Instr ret;
        ret.op = Opcode::Ret;
        ret.type = Type(Type::Kind::I64);
        ret.operands.push_back(Value::constInt(retVal));
        entry.instructions.push_back(ret);
        fn.blocks.push_back(std::move(entry));
    }

    return fn;
}

/// Create a function with parameters.
static Function makeI64FuncWithParams(const std::string &name,
                                      Linkage linkage,
                                      std::vector<Param> params) {
    Function fn;
    fn.name = name;
    fn.retType = Type(Type::Kind::I64);
    fn.params = std::move(params);
    fn.linkage = linkage;
    // No body for import
    return fn;
}

static Function makeI1Func(const std::string &name, Linkage linkage, bool retVal = true) {
    Function fn;
    fn.name = name;
    fn.retType = Type(Type::Kind::I1);
    fn.linkage = linkage;

    if (linkage != Linkage::Import) {
        BasicBlock entry;
        entry.label = "entry";
        Instr ret;
        ret.op = Opcode::Ret;
        ret.type = Type(Type::Kind::I1);
        ret.operands.push_back(Value::constBool(retVal));
        entry.instructions.push_back(ret);
        fn.blocks.push_back(std::move(entry));
    }

    return fn;
}

static Function makeCaller(const std::string &name,
                           const std::string &callee,
                           Linkage linkage = Linkage::Internal) {
    Function fn;
    fn.name = name;
    fn.retType = Type(Type::Kind::Void);
    fn.linkage = linkage;

    BasicBlock entry;
    entry.label = "entry";
    Instr call;
    call.op = Opcode::Call;
    call.type = Type(Type::Kind::Void);
    call.callee = callee;
    entry.instructions.push_back(std::move(call));
    Instr ret;
    ret.op = Opcode::Ret;
    entry.instructions.push_back(std::move(ret));
    fn.blocks.push_back(std::move(entry));
    return fn;
}

/// Check if a function with the given name exists in the module.
static bool hasFunction(const Module &m, const std::string &name) {
    return std::any_of(
        m.functions.begin(), m.functions.end(), [&](const Function &f) { return f.name == name; });
}

/// Count functions in the module.
static size_t countFunctions(const Module &m) {
    return m.functions.size();
}

static const Function *findFunction(const Module &m, const std::string &name) {
    auto it = std::find_if(
        m.functions.begin(), m.functions.end(), [&](const Function &f) { return f.name == name; });
    return it == m.functions.end() ? nullptr : &*it;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(ModuleLinker, SingleModulePassthrough) {
    Module m;
    m.target = "x86_64-unknown-linux-gnu";
    m.functions.push_back(makeI64Func("main", Linkage::Internal));

    std::vector<Module> modules;
    modules.push_back(std::move(m));

    auto result = il::link::linkModules(std::move(modules));
    ASSERT_TRUE(result.succeeded());
    EXPECT_TRUE(hasFunction(result.module, "main"));
    ASSERT_TRUE(result.module.target.has_value());
    EXPECT_EQ(*result.module.target, "x86_64-unknown-linux-gnu");
}

TEST(ModuleLinker, RejectsMixedVersionsAndTargets) {
    Module entry;
    entry.version = "0.3.0";
    entry.target = "x86_64-unknown-linux-gnu";
    entry.functions.push_back(makeVoidFunc("main", Linkage::Internal));

    Module other;
    other.version = "0.2.0";
    other.target = entry.target;
    auto versionResult = il::link::linkModules({entry, other});
    EXPECT_FALSE(versionResult.succeeded());
    ASSERT_FALSE(versionResult.errors.empty());
    EXPECT_CONTAINS(versionResult.errors.front(), "IL version mismatch");

    other.version = entry.version;
    other.target = "aarch64-apple-darwin";
    auto targetResult = il::link::linkModules({entry, other});
    EXPECT_FALSE(targetResult.succeeded());
    ASSERT_FALSE(targetResult.errors.empty());
    EXPECT_CONTAINS(targetResult.errors.front(), "target triple mismatch");
}

TEST(ModuleLinker, EntryGlobalWinsRegardlessOfInputOrder) {
    Module library;
    library.globals.push_back(Global{"state", Type(Type::Kind::I64), "1"});

    Module entry;
    entry.functions.push_back(makeVoidFunc("main", Linkage::Internal));
    entry.globals.push_back(Global{"state", Type(Type::Kind::I64), "2"});

    auto result = il::link::linkModules({library, entry});
    ASSERT_TRUE(result.succeeded());
    ASSERT_EQ(result.module.globals.size(), 2u);
    EXPECT_TRUE(std::any_of(result.module.globals.begin(),
                            result.module.globals.end(),
                            [](const Global &g) { return g.name == "state" && g.init == "2"; }));
    EXPECT_TRUE(std::any_of(result.module.globals.begin(),
                            result.module.globals.end(),
                            [](const Global &g) { return g.name != "state" && g.init == "1"; }));
}

TEST(ModuleLinker, RejectsInvalidInitializerSignature) {
    Module entry;
    entry.functions.push_back(makeVoidFunc("main", Linkage::Internal));
    Module library;
    library.functions.push_back(makeI64Func("library$init", Linkage::Internal, 1));
    library.functions.back().moduleInitializer = true;

    auto result = il::link::linkModules({entry, library});
    EXPECT_FALSE(result.succeeded());
    ASSERT_FALSE(result.errors.empty());
    EXPECT_CONTAINS(result.errors.front(), "invalid module initializer signature");
}

TEST(ModuleLinker, DoesNotGuessInitializersFromFunctionNames) {
    Module entry;
    entry.functions.push_back(makeVoidFunc("main", Linkage::Internal));
    Module library;
    library.functions.push_back(makeVoidFunc("ordinary$init", Linkage::Internal));

    auto result = il::link::linkModules({std::move(entry), std::move(library)});
    ASSERT_TRUE(result.succeeded());
    const Function *main = findFunction(result.module, "main");
    ASSERT_NE(main, nullptr);
    ASSERT_FALSE(main->blocks.empty());
    EXPECT_EQ(main->blocks.front().instructions.front().op, Opcode::Ret);
}

TEST(ModuleLinker, ResolvesGlobalImportsAndDropsDeclarationStubs) {
    Module entry;
    entry.functions.push_back(makeVoidFunc("main", Linkage::Internal));
    Global imported;
    imported.name = "shared";
    imported.type = Type(Type::Kind::I64);
    imported.linkage = Linkage::Import;
    entry.globals.push_back(imported);

    Module library;
    Global exported;
    exported.name = "shared";
    exported.type = Type(Type::Kind::I64);
    exported.linkage = Linkage::Export;
    exported.init = "42";
    exported.hasInitializer = true;
    library.globals.push_back(exported);

    auto result = il::link::linkModules({std::move(entry), std::move(library)});
    ASSERT_TRUE(result.succeeded());
    ASSERT_EQ(result.module.globals.size(), 1u);
    EXPECT_EQ(result.module.globals.front().name, "shared");
    EXPECT_EQ(result.module.globals.front().init, "42");
    EXPECT_EQ(result.module.globals.front().linkage, Linkage::Export);
}

TEST(ModuleLinker, RejectsUnresolvedAndMismatchedGlobalImports) {
    Module unresolvedEntry;
    unresolvedEntry.functions.push_back(makeVoidFunc("main", Linkage::Internal));
    Global missing;
    missing.name = "missing";
    missing.type = Type(Type::Kind::I64);
    missing.linkage = Linkage::Import;
    unresolvedEntry.globals.push_back(missing);
    auto unresolved = il::link::linkModules({std::move(unresolvedEntry)});
    EXPECT_FALSE(unresolved.succeeded());
    EXPECT_CONTAINS(unresolved.errors.front(), "unresolved global import");

    Module entry;
    entry.functions.push_back(makeVoidFunc("main", Linkage::Internal));
    Global imported;
    imported.name = "shared";
    imported.type = Type(Type::Kind::I64);
    imported.linkage = Linkage::Import;
    entry.globals.push_back(imported);
    Module library;
    Global exported;
    exported.name = "shared";
    exported.type = Type(Type::Kind::F64);
    exported.linkage = Linkage::Export;
    exported.init = "1.0";
    exported.hasInitializer = true;
    library.globals.push_back(exported);
    auto mismatch = il::link::linkModules({std::move(entry), std::move(library)});
    EXPECT_FALSE(mismatch.succeeded());
    EXPECT_CONTAINS(mismatch.errors.front(), "global signature mismatch");
}

TEST(ModuleLinker, RejectsVariadicBooleanThunk) {
    Module entry;
    entry.functions.push_back(makeVoidFunc("main", Linkage::Internal));
    Function imported = makeI64Func("isReady", Linkage::Import);
    imported.isVarArg = true;
    entry.functions.push_back(std::move(imported));

    Module library;
    Function exported = makeI1Func("isReady", Linkage::Export);
    exported.isVarArg = true;
    library.functions.push_back(std::move(exported));

    auto result = il::link::linkModules({entry, library});
    EXPECT_FALSE(result.succeeded());
    ASSERT_FALSE(result.errors.empty());
    EXPECT_CONTAINS(result.errors.front(), "signature mismatch");
}

TEST(ModuleLinker, TwoModulesExportImportResolved) {
    // Module A: entry module with main, imports "helper"
    Module a;
    a.functions.push_back(makeI64Func("main", Linkage::Internal));
    a.functions.push_back(makeI64Func("helper", Linkage::Import));

    // Module B: library with exported "helper"
    Module b;
    b.functions.push_back(makeI64Func("helper", Linkage::Export, 42));

    std::vector<Module> modules;
    modules.push_back(std::move(a));
    modules.push_back(std::move(b));

    auto result = il::link::linkModules(std::move(modules));
    ASSERT_TRUE(result.succeeded());
    EXPECT_TRUE(hasFunction(result.module, "main"));
    EXPECT_TRUE(hasFunction(result.module, "helper"));

    // The import stub should be dropped, so we have exactly 2 functions.
    EXPECT_EQ(countFunctions(result.module), 2u);
}

TEST(ModuleLinker, UnresolvedImportFails) {
    Module a;
    a.functions.push_back(makeI64Func("main", Linkage::Internal));
    a.functions.push_back(makeI64Func("missing", Linkage::Import));

    Module b;
    b.functions.push_back(makeVoidFunc("unrelated", Linkage::Export));

    std::vector<Module> modules;
    modules.push_back(std::move(a));
    modules.push_back(std::move(b));

    auto result = il::link::linkModules(std::move(modules));
    EXPECT_FALSE(result.succeeded());
    ASSERT_FALSE(result.errors.empty());
    EXPECT_TRUE(result.errors[0].find("missing") != std::string::npos);
}

TEST(ModuleLinker, DuplicateMainFails) {
    Module a;
    a.functions.push_back(makeI64Func("main", Linkage::Internal));

    Module b;
    b.functions.push_back(makeI64Func("main", Linkage::Internal));

    std::vector<Module> modules;
    modules.push_back(std::move(a));
    modules.push_back(std::move(b));

    auto result = il::link::linkModules(std::move(modules));
    EXPECT_FALSE(result.succeeded());
    ASSERT_FALSE(result.errors.empty());
    EXPECT_TRUE(result.errors[0].find("multiple modules define 'main'") != std::string::npos);
}

TEST(ModuleLinker, InternalNameCollisionPrefixed) {
    // Both modules have an Internal "helper" — non-entry one gets prefixed.
    Module a;
    a.functions.push_back(makeI64Func("main", Linkage::Internal));
    a.functions.push_back(makeVoidFunc("helper", Linkage::Internal));

    Module b;
    b.functions.push_back(makeVoidFunc("helper", Linkage::Internal));
    b.functions.push_back(makeI64Func("compute", Linkage::Export, 99));

    std::vector<Module> modules;
    modules.push_back(std::move(a));
    modules.push_back(std::move(b));

    auto result = il::link::linkModules(std::move(modules));
    ASSERT_TRUE(result.succeeded());
    EXPECT_TRUE(hasFunction(result.module, "helper"));    // Entry module's version
    EXPECT_TRUE(hasFunction(result.module, "m1$helper")); // Non-entry gets prefix
    EXPECT_TRUE(hasFunction(result.module, "compute"));
}

TEST(ModuleLinker, ExternsMergedAndDeduplicated) {
    Module a;
    a.functions.push_back(makeI64Func("main", Linkage::Internal));
    a.externs.push_back({"Zanna.Terminal.Say", Type(Type::Kind::Void), {Type(Type::Kind::Str)}});

    Module b;
    b.functions.push_back(makeVoidFunc("lib", Linkage::Export));
    b.externs.push_back({"Zanna.Terminal.Say", Type(Type::Kind::Void), {Type(Type::Kind::Str)}});

    std::vector<Module> modules;
    modules.push_back(std::move(a));
    modules.push_back(std::move(b));

    auto result = il::link::linkModules(std::move(modules));
    ASSERT_TRUE(result.succeeded());

    // Should be deduplicated to one extern.
    EXPECT_EQ(result.module.externs.size(), 1u);
}

TEST(ModuleLinker, ExternEffectAttributesAreIntersected) {
    Module a;
    a.functions.push_back(makeI64Func("main", Linkage::Internal));
    Extern readonlyExtern{"Zanna.Foo", Type(Type::Kind::I64), {Type(Type::Kind::Ptr)}};
    readonlyExtern.attrs().readonly = true;
    a.externs.push_back(readonlyExtern);

    Module b;
    b.functions.push_back(makeVoidFunc("lib", Linkage::Export));
    Extern nothrowExtern{"Zanna.Foo", Type(Type::Kind::I64), {Type(Type::Kind::Ptr)}};
    nothrowExtern.attrs().nothrow = true;
    b.externs.push_back(nothrowExtern);

    std::vector<Module> modules;
    modules.push_back(std::move(a));
    modules.push_back(std::move(b));

    auto result = il::link::linkModules(std::move(modules));
    ASSERT_TRUE(result.succeeded());
    ASSERT_EQ(result.module.externs.size(), 1u);
    EXPECT_FALSE(result.module.externs.front().attrs().readonly);
    EXPECT_FALSE(result.module.externs.front().attrs().nothrow);
}

TEST(ModuleLinker, ExternSignatureMismatchFails) {
    Module a;
    a.functions.push_back(makeI64Func("main", Linkage::Internal));
    a.externs.push_back({"Zanna.Foo", Type(Type::Kind::Void), {Type(Type::Kind::I64)}});

    Module b;
    b.functions.push_back(makeVoidFunc("lib", Linkage::Export));
    // Same name but different return type
    b.externs.push_back({"Zanna.Foo", Type(Type::Kind::I64), {Type(Type::Kind::I64)}});

    std::vector<Module> modules;
    modules.push_back(std::move(a));
    modules.push_back(std::move(b));

    auto result = il::link::linkModules(std::move(modules));
    EXPECT_FALSE(result.succeeded());
    EXPECT_TRUE(result.errors[0].find("extern signature mismatch") != std::string::npos);
}

TEST(ModuleLinker, GlobalsMerged) {
    Module a;
    a.functions.push_back(makeI64Func("main", Linkage::Internal));
    a.globals.push_back({"str0", Type(Type::Kind::Str), "hello"});

    Module b;
    b.functions.push_back(makeVoidFunc("lib", Linkage::Export));
    b.globals.push_back({"str1", Type(Type::Kind::Str), "world"});

    std::vector<Module> modules;
    modules.push_back(std::move(a));
    modules.push_back(std::move(b));

    auto result = il::link::linkModules(std::move(modules));
    ASSERT_TRUE(result.succeeded());
    EXPECT_EQ(result.module.globals.size(), 2u);
}

TEST(ModuleLinker, PreservesFirstSeenExternAndGlobalOrder) {
    Module entry;
    entry.functions.push_back(makeI64Func("main", Linkage::Internal));
    entry.externs.push_back({"Zanna.Zed", Type(Type::Kind::Void), {}});
    entry.externs.push_back({"Zanna.Alpha", Type(Type::Kind::Void), {}});
    entry.globals.push_back({"zed", Type(Type::Kind::I64), "1"});
    entry.globals.push_back({"alpha", Type(Type::Kind::I64), "2"});

    Module library;
    library.functions.push_back(makeVoidFunc("lib", Linkage::Export));
    library.externs.push_back({"Zanna.Middle", Type(Type::Kind::Void), {}});
    library.globals.push_back({"middle", Type(Type::Kind::I64), "3"});

    std::vector<Module> modules;
    modules.push_back(std::move(entry));
    modules.push_back(std::move(library));

    auto result = il::link::linkModules(std::move(modules));
    ASSERT_TRUE(result.succeeded());
    ASSERT_EQ(result.module.externs.size(), 3u);
    EXPECT_EQ(result.module.externs[0].name, "Zanna.Zed");
    EXPECT_EQ(result.module.externs[1].name, "Zanna.Alpha");
    EXPECT_EQ(result.module.externs[2].name, "Zanna.Middle");
    ASSERT_EQ(result.module.globals.size(), 3u);
    EXPECT_EQ(result.module.globals[0].name, "zed");
    EXPECT_EQ(result.module.globals[1].name, "alpha");
    EXPECT_EQ(result.module.globals[2].name, "middle");
}

TEST(ModuleLinker, GlobalCollisionUsesStableNumericSuffix) {
    Module entry;
    entry.functions.push_back(makeI64Func("main", Linkage::Internal));
    entry.globals.push_back({"value", Type(Type::Kind::I64), "1"});
    entry.globals.push_back({"m1$value", Type(Type::Kind::I64), "2"});

    Module library;
    library.functions.push_back(makeVoidFunc("lib", Linkage::Export));
    library.globals.push_back({"value", Type(Type::Kind::I64), "3"});

    std::vector<Module> modules;
    modules.push_back(std::move(entry));
    modules.push_back(std::move(library));

    auto result = il::link::linkModules(std::move(modules));
    ASSERT_TRUE(result.succeeded());
    ASSERT_EQ(result.module.globals.size(), 3u);
    EXPECT_EQ(result.module.globals[2].name, "m1$value$1");
}

TEST(ModuleLinker, EmptyModuleListFails) {
    std::vector<Module> modules;
    auto result = il::link::linkModules(std::move(modules));
    EXPECT_FALSE(result.succeeded());
}

TEST(ModuleLinker, NoMainFails) {
    Module a;
    a.functions.push_back(makeVoidFunc("notMain", Linkage::Export));

    Module b;
    b.functions.push_back(makeVoidFunc("alsoNotMain", Linkage::Export));

    std::vector<Module> modules;
    modules.push_back(std::move(a));
    modules.push_back(std::move(b));

    auto result = il::link::linkModules(std::move(modules));
    EXPECT_FALSE(result.succeeded());
    EXPECT_TRUE(result.errors[0].find("no module defines 'main'") != std::string::npos);
}

TEST(ModuleLinker, SingleModuleUnresolvedImportFails) {
    Module a;
    a.functions.push_back(makeI64Func("main", Linkage::Internal));
    a.functions.push_back(makeI64Func("missing", Linkage::Import));

    std::vector<Module> modules;
    modules.push_back(std::move(a));

    auto result = il::link::linkModules(std::move(modules));
    EXPECT_FALSE(result.succeeded());
    ASSERT_FALSE(result.errors.empty());
    EXPECT_TRUE(result.errors[0].find("missing") != std::string::npos);
}

TEST(ModuleLinker, DuplicateExportFails) {
    Module a;
    a.functions.push_back(makeI64Func("main", Linkage::Internal));
    a.functions.push_back(makeI64Func("helper", Linkage::Export));

    Module b;
    b.functions.push_back(makeI64Func("helper", Linkage::Export));

    std::vector<Module> modules;
    modules.push_back(std::move(a));
    modules.push_back(std::move(b));

    auto result = il::link::linkModules(std::move(modules));
    EXPECT_FALSE(result.succeeded());
    ASSERT_FALSE(result.errors.empty());
    EXPECT_TRUE(result.errors[0].find("duplicate export") != std::string::npos);
}

TEST(ModuleLinker, ImportSignatureMismatchFails) {
    Module a;
    a.functions.push_back(makeI64Func("main", Linkage::Internal));
    a.functions.push_back(makeI64Func("helper", Linkage::Import));

    Module b;
    b.functions.push_back(makeVoidFunc("helper", Linkage::Export));

    std::vector<Module> modules;
    modules.push_back(std::move(a));
    modules.push_back(std::move(b));

    auto result = il::link::linkModules(std::move(modules));
    EXPECT_FALSE(result.succeeded());
    ASSERT_FALSE(result.errors.empty());
    EXPECT_TRUE(result.errors[0].find("signature mismatch") != std::string::npos);
}

TEST(ModuleLinker, ImportVarArgMismatchFails) {
    Module a;
    a.functions.push_back(makeI64Func("main", Linkage::Internal));
    Function imported =
        makeI64FuncWithParams("helper", Linkage::Import, {Param{"x", Type(Type::Kind::I64), 0}});
    imported.isVarArg = false;
    a.functions.push_back(std::move(imported));

    Module b;
    Function exported =
        makeI64FuncWithParams("helper", Linkage::Export, {Param{"x", Type(Type::Kind::I64), 0}});
    exported.isVarArg = true;
    BasicBlock entry;
    entry.label = "entry";
    Instr ret;
    ret.op = Opcode::Ret;
    ret.type = Type(Type::Kind::I64);
    ret.operands.push_back(Value::constInt(0));
    entry.instructions.push_back(std::move(ret));
    exported.blocks.push_back(std::move(entry));
    b.functions.push_back(std::move(exported));

    std::vector<Module> modules;
    modules.push_back(std::move(a));
    modules.push_back(std::move(b));

    auto result = il::link::linkModules(std::move(modules));
    EXPECT_FALSE(result.succeeded());
    ASSERT_FALSE(result.errors.empty());
    EXPECT_TRUE(result.errors[0].find("signature mismatch") != std::string::npos);
}

TEST(ModuleLinker, BooleanThunkIntegratedForImportCalls) {
    Module a;
    a.functions.push_back(makeCaller("main", "isReady"));
    a.functions.push_back(makeI64Func("isReady", Linkage::Import));

    Module b;
    b.functions.push_back(makeI1Func("isReady", Linkage::Export));

    std::vector<Module> modules;
    modules.push_back(std::move(a));
    modules.push_back(std::move(b));

    auto result = il::link::linkModules(std::move(modules));
    ASSERT_TRUE(result.succeeded());
    EXPECT_TRUE(hasFunction(result.module, "isReady$bool_thunk"));

    auto mainIt = std::find_if(result.module.functions.begin(),
                               result.module.functions.end(),
                               [](const Function &fn) { return fn.name == "main"; });
    ASSERT_TRUE(mainIt != result.module.functions.end());
    ASSERT_FALSE(mainIt->blocks.empty());
    ASSERT_FALSE(mainIt->blocks.front().instructions.empty());
    EXPECT_EQ(mainIt->blocks.front().instructions.front().callee, "isReady$bool_thunk");
}

TEST(ModuleLinker, InternalRenameIsModuleLocal) {
    Module a;
    a.functions.push_back(makeVoidFunc("main", Linkage::Internal));
    a.functions.push_back(makeVoidFunc("helper", Linkage::Internal));

    Module b;
    b.functions.push_back(makeCaller("lib", "helper", Linkage::Export));
    b.functions.push_back(makeVoidFunc("helper", Linkage::Internal));

    std::vector<Module> modules;
    modules.push_back(std::move(a));
    modules.push_back(std::move(b));

    auto result = il::link::linkModules(std::move(modules));
    ASSERT_TRUE(result.succeeded());
    EXPECT_TRUE(hasFunction(result.module, "helper"));
    EXPECT_TRUE(hasFunction(result.module, "m1$helper"));

    auto libIt = std::find_if(result.module.functions.begin(),
                              result.module.functions.end(),
                              [](const Function &fn) { return fn.name == "lib"; });
    ASSERT_TRUE(libIt != result.module.functions.end());
    ASSERT_FALSE(libIt->blocks.empty());
    ASSERT_FALSE(libIt->blocks.front().instructions.empty());
    EXPECT_EQ(libIt->blocks.front().instructions.front().callee, "m1$helper");
}

TEST(ModuleLinker, RejectsDuplicateFunctionsBeforeConstructingRenameMaps) {
    Module entry;
    entry.functions.push_back(makeVoidFunc("main", Linkage::Internal));

    Module library;
    library.functions.push_back(makeVoidFunc("helper", Linkage::Internal));
    library.functions.push_back(makeVoidFunc("helper", Linkage::Internal));

    auto result = il::link::linkModules({std::move(entry), std::move(library)});
    EXPECT_FALSE(result.succeeded());
    ASSERT_FALSE(result.errors.empty());
    EXPECT_CONTAINS(result.errors.front(), "duplicate function in module 1: @helper");
}

TEST(ModuleLinker, InternalRenameUpdatesIndirectFunctionAddresses) {
    Module a;
    a.functions.push_back(makeVoidFunc("main", Linkage::Internal));
    a.functions.push_back(makeVoidFunc("helper", Linkage::Internal));

    Module b;
    Function lib;
    lib.name = "lib";
    lib.retType = Type(Type::Kind::Void);
    lib.linkage = Linkage::Export;
    BasicBlock entry;
    entry.label = "entry";
    Instr indirect;
    indirect.op = Opcode::CallIndirect;
    indirect.type = Type(Type::Kind::Void);
    indirect.operands = {Value::global("helper")};
    entry.instructions.push_back(std::move(indirect));
    Instr ret;
    ret.op = Opcode::Ret;
    entry.instructions.push_back(std::move(ret));
    entry.terminated = true;
    lib.blocks.push_back(std::move(entry));
    b.functions.push_back(std::move(lib));
    b.functions.push_back(makeVoidFunc("helper", Linkage::Internal));

    std::vector<Module> modules;
    modules.push_back(std::move(a));
    modules.push_back(std::move(b));

    auto result = il::link::linkModules(std::move(modules));
    ASSERT_TRUE(result.succeeded());
    EXPECT_TRUE(hasFunction(result.module, "m1$helper"));

    const Function *linkedLib = findFunction(result.module, "lib");
    ASSERT_NE(linkedLib, nullptr);
    ASSERT_FALSE(linkedLib->blocks.empty());
    ASSERT_FALSE(linkedLib->blocks.front().instructions.empty());
    const Instr &linkedCall = linkedLib->blocks.front().instructions.front();
    ASSERT_EQ(linkedCall.op, Opcode::CallIndirect);
    ASSERT_EQ(linkedCall.operands.size(), 1u);
    ASSERT_EQ(linkedCall.operands.front().kind, Value::Kind::GlobalAddr);
    EXPECT_EQ(linkedCall.operands.front().str, "m1$helper");
}

TEST(ModuleLinker, InternalRenameUpdatesFunctionAddressesInBranchArgs) {
    Module a;
    a.functions.push_back(makeVoidFunc("main", Linkage::Internal));
    a.functions.push_back(makeVoidFunc("helper", Linkage::Internal));

    Module b;
    Function lib;
    lib.name = "lib";
    lib.retType = Type(Type::Kind::Void);
    lib.linkage = Linkage::Export;

    BasicBlock entry;
    entry.label = "entry";
    Instr br;
    br.op = Opcode::Br;
    br.labels = {"next"};
    br.brArgs = {{Value::global("helper")}};
    entry.instructions.push_back(std::move(br));
    entry.terminated = true;

    BasicBlock next;
    next.label = "next";
    Param callee{"callee", Type(Type::Kind::Ptr), 0};
    next.params.push_back(callee);
    Instr indirect;
    indirect.op = Opcode::CallIndirect;
    indirect.type = Type(Type::Kind::Void);
    indirect.operands = {Value::temp(callee.id)};
    next.instructions.push_back(std::move(indirect));
    Instr ret;
    ret.op = Opcode::Ret;
    next.instructions.push_back(std::move(ret));
    next.terminated = true;

    lib.blocks.push_back(std::move(entry));
    lib.blocks.push_back(std::move(next));
    b.functions.push_back(std::move(lib));
    b.functions.push_back(makeVoidFunc("helper", Linkage::Internal));

    std::vector<Module> modules;
    modules.push_back(std::move(a));
    modules.push_back(std::move(b));

    auto result = il::link::linkModules(std::move(modules));
    ASSERT_TRUE(result.succeeded());
    EXPECT_TRUE(hasFunction(result.module, "m1$helper"));

    const Function *linkedLib = findFunction(result.module, "lib");
    ASSERT_NE(linkedLib, nullptr);
    ASSERT_GE(linkedLib->blocks.size(), 1u);
    ASSERT_FALSE(linkedLib->blocks.front().instructions.empty());
    const Instr &linkedBr = linkedLib->blocks.front().instructions.front();
    ASSERT_EQ(linkedBr.op, Opcode::Br);
    ASSERT_EQ(linkedBr.brArgs.size(), 1u);
    ASSERT_EQ(linkedBr.brArgs.front().size(), 1u);
    ASSERT_EQ(linkedBr.brArgs.front().front().kind, Value::Kind::GlobalAddr);
    EXPECT_EQ(linkedBr.brArgs.front().front().str, "m1$helper");
}

// --- ADR 0268: cross-language symbol resolution -----------------------------

TEST(ModuleLinker, ImportResolvesToUniqueCaseInsensitiveExport) {
    // Zanna BASIC upper-cases identifiers, so a BASIC import cannot match a
    // case-preserving export exactly. A unique case-folded match binds instead.
    Module a;
    a.functions.push_back(makeI64Func("main", Linkage::Internal));
    a.functions.push_back(makeI64Func("FACTORIAL", Linkage::Import));

    Module b;
    b.functions.push_back(makeI64Func("Factorial", Linkage::Export, 42));

    std::vector<Module> modules;
    modules.push_back(std::move(a));
    modules.push_back(std::move(b));

    auto result = il::link::linkModules(std::move(modules));
    ASSERT_TRUE(result.succeeded());
    // The definition keeps its own spelling; the import stub is dropped.
    EXPECT_TRUE(hasFunction(result.module, "Factorial"));
    EXPECT_FALSE(hasFunction(result.module, "FACTORIAL"));
    EXPECT_EQ(countFunctions(result.module), 2u);
}

TEST(ModuleLinker, CaseInsensitiveImportRewritesCallSites) {
    // Binding by case fold must also redirect callers to the real name,
    // otherwise the call would dangle after the import stub is dropped.
    Module a;
    a.functions.push_back(makeI64Func("main", Linkage::Internal));
    a.functions.push_back(makeVoidFunc("HELPER", Linkage::Import));
    a.functions.push_back(makeCaller("useHelper", "HELPER"));

    Module b;
    b.functions.push_back(makeVoidFunc("Helper", Linkage::Export));

    std::vector<Module> modules;
    modules.push_back(std::move(a));
    modules.push_back(std::move(b));

    auto result = il::link::linkModules(std::move(modules));
    ASSERT_TRUE(result.succeeded());
    const Function *caller = findFunction(result.module, "useHelper");
    ASSERT_TRUE(caller != nullptr);
    ASSERT_FALSE(caller->blocks.empty());
    ASSERT_FALSE(caller->blocks.front().instructions.empty());
    EXPECT_EQ(caller->blocks.front().instructions.front().callee, std::string("Helper"));
}

TEST(ModuleLinker, AmbiguousCaseInsensitiveImportFails) {
    // Two definitions differing only by case must not be resolved arbitrarily.
    Module a;
    a.functions.push_back(makeI64Func("main", Linkage::Internal));
    a.functions.push_back(makeI64Func("VALUE", Linkage::Import));

    Module b;
    b.functions.push_back(makeI64Func("Value", Linkage::Export, 1));
    b.functions.push_back(makeI64Func("value", Linkage::Export, 2));

    std::vector<Module> modules;
    modules.push_back(std::move(a));
    modules.push_back(std::move(b));

    auto result = il::link::linkModules(std::move(modules));
    EXPECT_FALSE(result.succeeded());
    ASSERT_FALSE(result.errors.empty());
    EXPECT_TRUE(result.errors[0].find("ambiguous import") != std::string::npos);
}

TEST(ModuleLinker, ExactMatchWinsOverCaseInsensitiveCandidate) {
    // The fallback is consulted only after exact resolution fails, so an exact
    // match must never be diverted to a differently-cased definition.
    Module a;
    a.functions.push_back(makeI64Func("main", Linkage::Internal));
    a.functions.push_back(makeI64Func("Value", Linkage::Import));

    Module b;
    b.functions.push_back(makeI64Func("Value", Linkage::Export, 1));
    b.functions.push_back(makeI64Func("VALUE", Linkage::Export, 2));

    std::vector<Module> modules;
    modules.push_back(std::move(a));
    modules.push_back(std::move(b));

    auto result = il::link::linkModules(std::move(modules));
    ASSERT_TRUE(result.succeeded());
    EXPECT_TRUE(hasFunction(result.module, "Value"));
    EXPECT_TRUE(hasFunction(result.module, "VALUE"));
}

int main() {
    return zanna_test::run_all_tests();
}
