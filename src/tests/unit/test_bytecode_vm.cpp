//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_bytecode_vm.cpp
// Purpose: Validate bytecode compilation, execution, traps, and runtime bridges.
// Key invariants:
//   - Switch and threaded dispatch produce the same observable results.
//   - Managed values survive asynchronous and native bridge ownership transfers.
// Ownership/Lifetime:
//   - Each test owns its bytecode module and releases runtime objects explicitly.
// Links: src/bytecode/BytecodeVM.cpp, src/bytecode/BytecodeCompiler.cpp
//
//===----------------------------------------------------------------------===//

#include "bytecode/BytecodeCompiler.hpp"
#include "bytecode/BytecodeModule.hpp"
#include "bytecode/BytecodeVM.hpp"
#include "il/build/IRBuilder.hpp"
#include "il/runtime/signatures/Registry.hpp"
#include "tests/common/PosixCompat.h"
#include "vm/RuntimeBridge.hpp"
#include "zanna/vm/RuntimeBridge.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using namespace il::core;
using namespace il::build;
using namespace zanna::bytecode;

using il::core::BasicBlock;
using il::runtime::signatures::make_signature;
using SigKind = il::runtime::signatures::SigParam::Kind;

struct rt_string_impl;
using rt_string = rt_string_impl *;
extern "C" const char *rt_string_cstr(rt_string s);
extern "C" void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);
extern "C" void rt_obj_set_finalizer(void *p, void (*fn)(void *));
extern "C" int32_t rt_obj_release_check0(void *p);
extern "C" void rt_obj_free(void *p);

namespace {

std::mutex g_asyncArgMutex;
std::condition_variable g_asyncArgCv;
bool g_asyncArgGateOpen = false;
std::atomic<int> g_asyncArgFinalized{0};

void async_arg_marker_finalizer(void *obj) {
    (void)obj;
    g_asyncArgFinalized.fetch_add(1, std::memory_order_relaxed);
}

void reset_async_arg_state() {
    {
        std::lock_guard<std::mutex> lock(g_asyncArgMutex);
        g_asyncArgGateOpen = false;
    }
    g_asyncArgFinalized.store(0, std::memory_order_relaxed);
}

void wait_async_arg_gate() {
    std::unique_lock<std::mutex> lock(g_asyncArgMutex);
    g_asyncArgCv.wait(lock, [] { return g_asyncArgGateOpen; });
}

void open_async_arg_gate() {
    {
        std::lock_guard<std::mutex> lock(g_asyncArgMutex);
        g_asyncArgGateOpen = true;
    }
    g_asyncArgCv.notify_all();
}

void make_async_arg_marker(void **args, void *result) {
    (void)args;
    void *marker = rt_obj_new_i64(0, 1);
    assert(marker != nullptr);
    rt_obj_set_finalizer(marker, async_arg_marker_finalizer);
    *reinterpret_cast<void **>(result) = marker;
}

void release_async_arg_marker(void **args, void *result) {
    (void)result;
    void *marker = (args && args[0]) ? *reinterpret_cast<void **>(args[0]) : nullptr;
    if (rt_obj_release_check0(marker))
        rt_obj_free(marker);
}

void count_async_arg_marker(void **args, void *result) {
    (void)args;
    *reinterpret_cast<int64_t *>(result) = g_asyncArgFinalized.load(std::memory_order_relaxed);
}

void wait_async_arg_native(void **args, void *result) {
    (void)args;
    (void)result;
    wait_async_arg_gate();
}

void open_async_arg_native(void **args, void *result) {
    (void)args;
    (void)result;
    open_async_arg_gate();
}

BytecodeModule compileAssumingVerified(const Module &ilModule) {
    BytecodeCompiler compiler;
    auto result = compiler.compileChecked(ilModule, nullptr, true);
    if (!result) {
        std::cerr << "bytecode test compile failed: " << result.error().message << "\n";
        assert(false);
    }
    return std::move(result.value());
}

} // namespace

/// Create a simple addition function for testing
/// func @add(i64 %a, i64 %b) -> i64
///   entry:
///     %result = iadd.ovf %a, %b
///     ret %result
static Module createAddModule() {
    Module m;
    IRBuilder b(m);

    // func @add(i64 %a, i64 %b) -> i64
    auto &fn = b.startFunction(
        "add",
        Type(Type::Kind::I64),
        {Param{"a", Type(Type::Kind::I64), 0}, Param{"b", Type(Type::Kind::I64), 1}});

    auto &entry = b.addBlock(fn, "entry");
    b.setInsertPoint(entry);

    // %result = iadd.ovf %a, %b
    Instr addInstr;
    addInstr.result = b.reserveTempId(); // temp 2 (after params 0 and 1)
    addInstr.op = Opcode::IAddOvf;
    addInstr.type = Type(Type::Kind::I64);
    addInstr.operands.push_back(Value::temp(0)); // param a
    addInstr.operands.push_back(Value::temp(1)); // param b
    addInstr.loc = {1, 1, 1};
    entry.instructions.push_back(addInstr);

    // ret %result
    Instr ret;
    ret.op = Opcode::Ret;
    ret.type = Type(Type::Kind::Void);
    ret.operands.push_back(Value::temp(*addInstr.result));
    ret.loc = {1, 1, 1};
    entry.instructions.push_back(ret);

    return m;
}

/// Create a function that tests conditional branching
/// func @abs(i64 %n) -> i64
///   entry:
///     %cmp = scmp_lt %n, 0
///     cbr %cmp, negative, positive
///   negative:
///     %neg = sub 0, %n
///     ret %neg
///   positive:
///     ret %n
static Module createAbsModule() {
    Module m;
    IRBuilder b(m);

    auto &fn =
        b.startFunction("abs", Type(Type::Kind::I64), {Param{"n", Type(Type::Kind::I64), 0}});

    // Create all blocks first to avoid reference invalidation from vector reallocation
    b.addBlock(fn, "entry");
    b.addBlock(fn, "negative");
    b.addBlock(fn, "positive");

    // Get fresh references after all blocks are added
    BasicBlock &entry = fn.blocks[0];
    BasicBlock &negative = fn.blocks[1];
    BasicBlock &positive = fn.blocks[2];

    // Entry block
    b.setInsertPoint(entry);

    // %cmp = scmp_lt %n, 0
    Instr cmpInstr;
    cmpInstr.result = b.reserveTempId();
    cmpInstr.op = Opcode::SCmpLT;
    cmpInstr.type = Type(Type::Kind::I1);
    cmpInstr.operands.push_back(Value::temp(0));
    cmpInstr.operands.push_back(Value::constInt(0));
    cmpInstr.loc = {1, 1, 1};
    entry.instructions.push_back(cmpInstr);

    // cbr %cmp, negative, positive
    b.cbr(Value::temp(*cmpInstr.result), negative, {}, positive, {});

    // Negative block: %neg = sub 0, %n; ret %neg
    b.setInsertPoint(negative);
    Instr subInstr;
    subInstr.result = b.reserveTempId();
    subInstr.op = Opcode::ISubOvf;
    subInstr.type = Type(Type::Kind::I64);
    subInstr.operands.push_back(Value::constInt(0));
    subInstr.operands.push_back(Value::temp(0));
    subInstr.loc = {1, 1, 1};
    negative.instructions.push_back(subInstr);

    Instr retNeg;
    retNeg.op = Opcode::Ret;
    retNeg.type = Type(Type::Kind::Void);
    retNeg.operands.push_back(Value::temp(*subInstr.result));
    retNeg.loc = {1, 1, 1};
    negative.instructions.push_back(retNeg);

    // Positive block: ret %n
    b.setInsertPoint(positive);
    Instr retPos;
    retPos.op = Opcode::Ret;
    retPos.type = Type(Type::Kind::Void);
    retPos.operands.push_back(Value::temp(0));
    retPos.loc = {1, 1, 1};
    positive.instructions.push_back(retPos);

    return m;
}

/// Create a recursive fibonacci function
/// func @fib(i64 %n) -> i64
///   entry:
///     %cmp = scmp_le %n, 1
///     cbr %cmp, base, recurse
///   base:
///     ret %n
///   recurse:
///     %nm1 = sub %n, 1
///     %fib1 = call @fib(%nm1)
///     %nm2 = sub %n, 2
///     %fib2 = call @fib(%nm2)
///     %result = add %fib1, %fib2
///     ret %result
static Module createFibModule() {
    Module m;
    IRBuilder b(m);

    auto &fn =
        b.startFunction("fib", Type(Type::Kind::I64), {Param{"n", Type(Type::Kind::I64), 0}});

    // Create all blocks first to avoid reference invalidation from vector reallocation
    b.addBlock(fn, "entry");
    b.addBlock(fn, "base");
    b.addBlock(fn, "recurse");

    // Get fresh references after all blocks are added
    BasicBlock &entry = fn.blocks[0];
    BasicBlock &base = fn.blocks[1];
    BasicBlock &recurse = fn.blocks[2];

    // Entry block: check if n <= 1
    b.setInsertPoint(entry);

    // %cmp = scmp_le %n, 1
    Instr cmpInstr;
    cmpInstr.result = b.reserveTempId(); // temp 1
    cmpInstr.op = Opcode::SCmpLE;
    cmpInstr.type = Type(Type::Kind::I1);
    cmpInstr.operands.push_back(Value::temp(0)); // param n
    cmpInstr.operands.push_back(Value::constInt(1));
    cmpInstr.loc = {1, 1, 1};
    entry.instructions.push_back(cmpInstr);

    // cbr %cmp, base, recurse
    b.cbr(Value::temp(*cmpInstr.result), base, {}, recurse, {});

    // Base case: return n
    b.setInsertPoint(base);
    Instr retBase;
    retBase.op = Opcode::Ret;
    retBase.type = Type(Type::Kind::Void);
    retBase.operands.push_back(Value::temp(0)); // Return param n
    retBase.loc = {1, 1, 1};
    base.instructions.push_back(retBase);

    // Recursive case
    b.setInsertPoint(recurse);

    // %nm1 = sub %n, 1
    Instr nm1Instr;
    nm1Instr.result = b.reserveTempId();
    nm1Instr.op = Opcode::ISubOvf;
    nm1Instr.type = Type(Type::Kind::I64);
    nm1Instr.operands.push_back(Value::temp(0));
    nm1Instr.operands.push_back(Value::constInt(1));
    nm1Instr.loc = {1, 1, 1};
    recurse.instructions.push_back(nm1Instr);

    // %fib1 = call @fib(%nm1)
    Instr call1;
    call1.result = b.reserveTempId();
    call1.op = Opcode::Call;
    call1.type = Type(Type::Kind::I64);
    call1.callee = "fib";
    call1.operands.push_back(Value::temp(*nm1Instr.result));
    call1.loc = {1, 1, 1};
    recurse.instructions.push_back(call1);

    // %nm2 = sub %n, 2
    Instr nm2Instr;
    nm2Instr.result = b.reserveTempId();
    nm2Instr.op = Opcode::ISubOvf;
    nm2Instr.type = Type(Type::Kind::I64);
    nm2Instr.operands.push_back(Value::temp(0));
    nm2Instr.operands.push_back(Value::constInt(2));
    nm2Instr.loc = {1, 1, 1};
    recurse.instructions.push_back(nm2Instr);

    // %fib2 = call @fib(%nm2)
    Instr call2;
    call2.result = b.reserveTempId();
    call2.op = Opcode::Call;
    call2.type = Type(Type::Kind::I64);
    call2.callee = "fib";
    call2.operands.push_back(Value::temp(*nm2Instr.result));
    call2.loc = {1, 1, 1};
    recurse.instructions.push_back(call2);

    // %result = add %fib1, %fib2
    Instr addInstr;
    addInstr.result = b.reserveTempId();
    addInstr.op = Opcode::IAddOvf;
    addInstr.type = Type(Type::Kind::I64);
    addInstr.operands.push_back(Value::temp(*call1.result));
    addInstr.operands.push_back(Value::temp(*call2.result));
    addInstr.loc = {1, 1, 1};
    recurse.instructions.push_back(addInstr);

    // ret %result
    Instr retRecurse;
    retRecurse.op = Opcode::Ret;
    retRecurse.type = Type(Type::Kind::Void);
    retRecurse.operands.push_back(Value::temp(*addInstr.result));
    retRecurse.loc = {1, 1, 1};
    recurse.instructions.push_back(retRecurse);

    return m;
}

/// Create a function that calls enough distinct native symbols to require a
/// native function index wider than 8 bits.
static Module createWideNativeIndexModule() {
    Module m;
    IRBuilder b(m);

    auto &fn = b.startFunction("call_wide_native", Type(Type::Kind::I64), {});
    auto &entry = b.addBlock(fn, "entry");
    b.setInsertPoint(entry);

    uint32_t lastResult = 0;
    for (uint32_t i = 0; i <= 256; ++i) {
        Instr call;
        call.result = b.reserveTempId();
        call.op = Opcode::Call;
        call.type = Type(Type::Kind::I64);
        call.callee = "native_" + std::to_string(i);
        call.loc = {1, 1, 1};
        entry.instructions.push_back(call);
        lastResult = *call.result;
    }

    Instr ret;
    ret.op = Opcode::Ret;
    ret.type = Type(Type::Kind::Void);
    ret.operands.push_back(Value::temp(lastResult));
    ret.loc = {1, 1, 1};
    entry.instructions.push_back(ret);

    return m;
}

/// @brief Element families covered by the dedicated numeric array fast-path opcodes.
enum class ArrayFastElementKind { I32, I64, F64 };

/// @brief Return the IL value type used by an array fast-path family.
/// @param kind Numeric array element family under test.
/// @return `f64` for floating arrays and `i64` for integer array runtime accessors.
static Type arrayFastValueType(ArrayFastElementKind kind) {
    return kind == ArrayFastElementKind::F64 ? Type(Type::Kind::F64) : Type(Type::Kind::I64);
}

/// @brief Return the runtime getter helper name that should compile to an `ARR_*_GET_FAST` opcode.
/// @param kind Numeric array element family under test.
/// @return Runtime helper name recognized by the bytecode compiler fast-path lowering.
static const char *arrayFastGetCallee(ArrayFastElementKind kind) {
    switch (kind) {
        case ArrayFastElementKind::I32:
            return "rt_arr_i32_get_fast";
        case ArrayFastElementKind::I64:
            return "rt_arr_i64_get_fast";
        case ArrayFastElementKind::F64:
            return "rt_arr_f64_get_fast";
    }
    return "";
}

/// @brief Return the runtime setter helper name that should compile to an `ARR_*_SET_FAST` opcode.
/// @param kind Numeric array element family under test.
/// @return Runtime helper name recognized by the bytecode compiler fast-path lowering.
static const char *arrayFastSetCallee(ArrayFastElementKind kind) {
    switch (kind) {
        case ArrayFastElementKind::I32:
            return "rt_arr_i32_set_fast";
        case ArrayFastElementKind::I64:
            return "rt_arr_i64_set_fast";
        case ArrayFastElementKind::F64:
            return "rt_arr_f64_set_fast";
    }
    return "";
}

/// @brief Return the bytecode getter opcode expected for an array fast-path family.
/// @param kind Numeric array element family under test.
/// @return Dedicated bytecode load opcode emitted for the runtime helper.
static BCOpcode arrayFastGetOpcode(ArrayFastElementKind kind) {
    switch (kind) {
        case ArrayFastElementKind::I32:
            return BCOpcode::ARR_I32_GET_FAST;
        case ArrayFastElementKind::I64:
            return BCOpcode::ARR_I64_GET_FAST;
        case ArrayFastElementKind::F64:
            return BCOpcode::ARR_F64_GET_FAST;
    }
    return BCOpcode::NOP;
}

/// @brief Return the bytecode setter opcode expected for an array fast-path family.
/// @param kind Numeric array element family under test.
/// @return Dedicated bytecode store opcode emitted for the runtime helper.
static BCOpcode arrayFastSetOpcode(ArrayFastElementKind kind) {
    switch (kind) {
        case ArrayFastElementKind::I32:
            return BCOpcode::ARR_I32_SET_FAST;
        case ArrayFastElementKind::I64:
            return BCOpcode::ARR_I64_SET_FAST;
        case ArrayFastElementKind::F64:
            return BCOpcode::ARR_F64_SET_FAST;
    }
    return BCOpcode::NOP;
}

/// @brief Build a typed IL module that stores to and loads from one fast array element.
/// @param kind Numeric array element family under test.
/// @return Module whose calls should lower to the matching `ARR_*_FAST` opcode pair.
static Module createArrayFastPathModule(ArrayFastElementKind kind) {
    Module m;
    IRBuilder b(m);

    auto &fn = b.startFunction("main", arrayFastValueType(kind), {Param{"arr", Type(Type::Kind::Ptr), 0}});
    auto &entry = b.addBlock(fn, "entry");
    b.setInsertPoint(entry);

    Instr set;
    set.op = Opcode::Call;
    set.type = Type(Type::Kind::Void);
    set.callee = arrayFastSetCallee(kind);
    set.operands.push_back(Value::temp(0));
    set.operands.push_back(Value::constInt(1));
    if (kind == ArrayFastElementKind::F64) {
        set.operands.push_back(Value::constFloat(123.5));
    } else {
        set.operands.push_back(Value::constInt(kind == ArrayFastElementKind::I32 ? 123 : 1234567890123LL));
    }
    set.loc = {1, 1, 1};
    entry.instructions.push_back(set);

    Instr get;
    get.result = b.reserveTempId();
    get.op = Opcode::Call;
    get.type = arrayFastValueType(kind);
    get.callee = arrayFastGetCallee(kind);
    get.operands.push_back(Value::temp(0));
    get.operands.push_back(Value::constInt(1));
    get.loc = {1, 1, 1};
    entry.instructions.push_back(get);

    Instr ret;
    ret.op = Opcode::Ret;
    ret.type = Type(Type::Kind::Void);
    ret.operands.push_back(Value::temp(*get.result));
    ret.loc = {1, 1, 1};
    entry.instructions.push_back(ret);

    return m;
}

/// @brief Build direct bytecode that forces an `ARR_*_GET_FAST` address overflow.
/// @param kind Numeric array element family under test.
/// @return Bytecode module that traps before dereferencing the supplied array pointer.
static BytecodeModule createArrayFastOverflowModule(ArrayFastElementKind kind) {
    BytecodeModule bcModule;
    bcModule.magic = kBytecodeModuleMagic;
    bcModule.version = kBytecodeVersion;
    bcModule.flags = 0;

    BytecodeFunction fn;
    fn.name = "main";
    fn.numParams = 1;
    fn.numLocals = 1;
    fn.maxStack = 2;
    fn.hasReturn = true;
    fn.localIsString = {0};
    fn.code.push_back(encodeOp8(BCOpcode::LOAD_LOCAL, 0));
    fn.code.push_back(encodeOp8(BCOpcode::LOAD_I8, -1));
    fn.code.push_back(encodeOp(arrayFastGetOpcode(kind)));
    fn.code.push_back(encodeOp(BCOpcode::RETURN));

    bcModule.addFunction(std::move(fn));
    return bcModule;
}

/// @brief Assert that a compiled function contains both typed array fast opcodes.
/// @param function Function whose bytecode stream will be scanned.
/// @param kind Numeric array element family expected in the bytecode stream.
static void assertArrayFastOpcodesPresent(const BytecodeFunction &function, ArrayFastElementKind kind) {
    bool sawSet = false;
    bool sawGet = false;
    for (uint32_t word : function.code) {
        sawSet = sawSet || decodeOpcode(word) == arrayFastSetOpcode(kind);
        sawGet = sawGet || decodeOpcode(word) == arrayFastGetOpcode(kind);
    }
    assert(sawSet);
    assert(sawGet);
}

/// @brief Build a bytecode-only module that exercises owned string stack operations.
/// @details Each function leaves an `i1` result from `Zanna.String.Equals`; this
///          keeps returned runtime strings inside the VM so argument release and
///          stack-helper ownership transfers must balance correctly before the
///          test can halt.
/// @return Module containing one function for each string-owning stack opcode family.
static BytecodeModule createStringStackOpsModule() {
    BytecodeModule bcModule;
    bcModule.magic = kBytecodeModuleMagic;
    bcModule.version = kBytecodeVersion;
    bcModule.flags = 0;

    const uint16_t aIdx = static_cast<uint16_t>(bcModule.addString("A"));
    const uint16_t bIdx = static_cast<uint16_t>(bcModule.addString("B"));
    const uint16_t cIdx = static_cast<uint16_t>(bcModule.addString("C"));
    const uint16_t aaIdx = static_cast<uint16_t>(bcModule.addString("AA"));
    const uint16_t baIdx = static_cast<uint16_t>(bcModule.addString("BA"));
    const uint16_t cabIdx = static_cast<uint16_t>(bcModule.addString("CAB"));
    const uint16_t ababIdx = static_cast<uint16_t>(bcModule.addString("ABAB"));
    const uint16_t concatIdx =
        static_cast<uint16_t>(bcModule.addNativeFunc("Zanna.String.Concat", 2, true));
    const uint16_t equalsIdx =
        static_cast<uint16_t>(bcModule.addNativeFunc("Zanna.String.Equals", 2, true));

    auto appendEqualsReturn = [&](BytecodeFunction &fn, uint16_t expectedIdx) {
        fn.code.push_back(encodeOp16(BCOpcode::LOAD_STR, expectedIdx));
        fn.code.push_back(encodeOp8_16(BCOpcode::CALL_NATIVE, 2, equalsIdx));
        fn.code.push_back(encodeOp(BCOpcode::RETURN));
    };

    auto addCase = [&](std::string name, std::vector<uint32_t> code, uint16_t expectedIdx) {
        BytecodeFunction fn;
        fn.name = std::move(name);
        fn.numParams = 0;
        fn.numLocals = 0;
        fn.maxStack = 6;
        fn.hasReturn = true;
        fn.code = std::move(code);
        appendEqualsReturn(fn, expectedIdx);
        bcModule.addFunction(std::move(fn));
    };

    addCase("dup_string",
            {encodeOp16(BCOpcode::LOAD_STR, aIdx),
             encodeOp(BCOpcode::DUP),
             encodeOp8_16(BCOpcode::CALL_NATIVE, 2, concatIdx)},
            aaIdx);

    addCase("dup2_string",
            {encodeOp16(BCOpcode::LOAD_STR, aIdx),
             encodeOp16(BCOpcode::LOAD_STR, bIdx),
             encodeOp(BCOpcode::DUP2),
             encodeOp8_16(BCOpcode::CALL_NATIVE, 2, concatIdx),
             encodeOp8_16(BCOpcode::CALL_NATIVE, 2, concatIdx),
             encodeOp8_16(BCOpcode::CALL_NATIVE, 2, concatIdx)},
            ababIdx);

    addCase("swap_string",
            {encodeOp16(BCOpcode::LOAD_STR, aIdx),
             encodeOp16(BCOpcode::LOAD_STR, bIdx),
             encodeOp(BCOpcode::SWAP),
             encodeOp8_16(BCOpcode::CALL_NATIVE, 2, concatIdx)},
            baIdx);

    addCase("rot3_string",
            {encodeOp16(BCOpcode::LOAD_STR, aIdx),
             encodeOp16(BCOpcode::LOAD_STR, bIdx),
             encodeOp16(BCOpcode::LOAD_STR, cIdx),
             encodeOp(BCOpcode::ROT3),
             encodeOp8_16(BCOpcode::CALL_NATIVE, 2, concatIdx),
             encodeOp8_16(BCOpcode::CALL_NATIVE, 2, concatIdx)},
            cabIdx);

    return bcModule;
}

static Module createBranchArgumentSwapModule() {
    Module m;

    Function fn;
    fn.name = "branch_arg_swap";
    fn.retType = Type(Type::Kind::I64);
    fn.params = {Param{"a", Type(Type::Kind::I64), 0}, Param{"b", Type(Type::Kind::I64), 1}};

    BasicBlock entry;
    entry.label = "entry";
    Instr enter;
    enter.op = Opcode::Br;
    enter.type = Type(Type::Kind::Void);
    enter.labels.push_back("loop");
    enter.brArgs.push_back({Value::temp(0), Value::temp(1), Value::constInt(1)});
    enter.loc = {1, 1, 1};
    entry.instructions.push_back(enter);
    entry.terminated = true;

    BasicBlock loop;
    loop.label = "loop";
    loop.params = {Param{"x", Type(Type::Kind::I64), 2},
                   Param{"y", Type(Type::Kind::I64), 3},
                   Param{"n", Type(Type::Kind::I64), 4}};
    Instr cmp;
    cmp.result = 5;
    cmp.op = Opcode::ICmpNe;
    cmp.type = Type(Type::Kind::I1);
    cmp.operands.push_back(Value::temp(4));
    cmp.operands.push_back(Value::constInt(0));
    cmp.loc = {1, 1, 1};
    loop.instructions.push_back(cmp);
    Instr branch;
    branch.op = Opcode::CBr;
    branch.type = Type(Type::Kind::Void);
    branch.operands.push_back(Value::temp(5));
    branch.labels.push_back("body");
    branch.labels.push_back("done");
    branch.brArgs.push_back({});
    branch.brArgs.push_back({Value::temp(2), Value::temp(3)});
    branch.loc = {1, 1, 1};
    loop.instructions.push_back(branch);
    loop.terminated = true;

    BasicBlock body;
    body.label = "body";
    Instr backedge;
    backedge.op = Opcode::Br;
    backedge.type = Type(Type::Kind::Void);
    backedge.labels.push_back("loop");
    backedge.brArgs.push_back({Value::temp(3), Value::temp(2), Value::constInt(0)});
    backedge.loc = {1, 1, 1};
    body.instructions.push_back(backedge);
    body.terminated = true;

    BasicBlock done;
    done.label = "done";
    done.params = {Param{"out_x", Type(Type::Kind::I64), 6},
                   Param{"out_y", Type(Type::Kind::I64), 7}};
    Instr ret;
    ret.op = Opcode::Ret;
    ret.type = Type(Type::Kind::Void);
    ret.operands.push_back(Value::temp(7));
    ret.loc = {1, 1, 1};
    done.instructions.push_back(ret);
    done.terminated = true;

    fn.blocks.push_back(std::move(entry));
    fn.blocks.push_back(std::move(loop));
    fn.blocks.push_back(std::move(body));
    fn.blocks.push_back(std::move(done));
    fn.valueNames.resize(8);
    m.functions.push_back(std::move(fn));
    return m;
}

/// Create a function that exercises bytecode string ownership for memory slots.
static Module createStringFieldLifetimeModule() {
    Module m;
    IRBuilder b(m);

    auto &fn = b.startFunction("field_lifetime", Type(Type::Kind::I1), {});
    auto &entry = b.addBlock(fn, "entry");
    b.setInsertPoint(entry);

    Instr slot;
    slot.result = b.reserveTempId();
    slot.op = Opcode::Alloca;
    slot.type = Type(Type::Kind::Ptr);
    slot.operands.push_back(Value::constInt(8));
    slot.loc = {1, 1, 1};
    entry.instructions.push_back(slot);

    Instr empty;
    empty.result = b.reserveTempId();
    empty.op = Opcode::ConstStr;
    empty.type = Type(Type::Kind::Str);
    empty.operands.push_back(Value::constStr(""));
    empty.loc = {1, 1, 1};
    entry.instructions.push_back(empty);

    Instr storeInitial;
    storeInitial.op = Opcode::Store;
    storeInitial.type = Type(Type::Kind::Str);
    storeInitial.operands.push_back(Value::temp(*slot.result));
    storeInitial.operands.push_back(Value::temp(*empty.result));
    storeInitial.loc = {1, 1, 1};
    entry.instructions.push_back(storeInitial);

    Instr buddy;
    buddy.result = b.reserveTempId();
    buddy.op = Opcode::ConstStr;
    buddy.type = Type(Type::Kind::Str);
    buddy.operands.push_back(Value::constStr("Buddy"));
    buddy.loc = {1, 1, 1};
    entry.instructions.push_back(buddy);

    Instr oldValue;
    oldValue.result = b.reserveTempId();
    oldValue.op = Opcode::Load;
    oldValue.type = Type(Type::Kind::Str);
    oldValue.operands.push_back(Value::temp(*slot.result));
    oldValue.loc = {1, 1, 1};
    entry.instructions.push_back(oldValue);

    Instr retainBuddy;
    retainBuddy.op = Opcode::Call;
    retainBuddy.type = Type(Type::Kind::Void);
    retainBuddy.callee = "rt_str_retain_maybe";
    retainBuddy.operands.push_back(Value::temp(*buddy.result));
    retainBuddy.loc = {1, 1, 1};
    entry.instructions.push_back(retainBuddy);

    Instr storeBuddy;
    storeBuddy.op = Opcode::Store;
    storeBuddy.type = Type(Type::Kind::Str);
    storeBuddy.operands.push_back(Value::temp(*slot.result));
    storeBuddy.operands.push_back(Value::temp(*buddy.result));
    storeBuddy.loc = {1, 1, 1};
    entry.instructions.push_back(storeBuddy);

    Instr releaseOld;
    releaseOld.op = Opcode::Call;
    releaseOld.type = Type(Type::Kind::Void);
    releaseOld.callee = "rt_str_release_maybe";
    releaseOld.operands.push_back(Value::temp(*oldValue.result));
    releaseOld.loc = {1, 1, 1};
    entry.instructions.push_back(releaseOld);

    Instr current;
    current.result = b.reserveTempId();
    current.op = Opcode::Load;
    current.type = Type(Type::Kind::Str);
    current.operands.push_back(Value::temp(*slot.result));
    current.loc = {1, 1, 1};
    entry.instructions.push_back(current);

    Instr expected;
    expected.result = b.reserveTempId();
    expected.op = Opcode::ConstStr;
    expected.type = Type(Type::Kind::Str);
    expected.operands.push_back(Value::constStr("Buddy"));
    expected.loc = {1, 1, 1};
    entry.instructions.push_back(expected);

    Instr eq;
    eq.result = b.reserveTempId();
    eq.op = Opcode::Call;
    eq.type = Type(Type::Kind::I1);
    eq.callee = "Zanna.String.Equals";
    eq.operands.push_back(Value::temp(*current.result));
    eq.operands.push_back(Value::temp(*expected.result));
    eq.loc = {1, 1, 1};
    entry.instructions.push_back(eq);

    Instr ret;
    ret.op = Opcode::Ret;
    ret.type = Type(Type::Kind::Void);
    ret.operands.push_back(Value::temp(*eq.result));
    ret.loc = {1, 1, 1};
    entry.instructions.push_back(ret);

    return m;
}

/// Create a function that stores through a module-global address and reads it back.
static Module createGlobalAddressModule() {
    Module m;
    m.globals.push_back({"counter", Type(Type::Kind::I64), "41"});

    Function fn;
    fn.name = "global_addr";
    fn.retType = Type(Type::Kind::I64);

    BasicBlock entry;
    entry.label = "entry";

    Instr ptr;
    ptr.result = 0;
    ptr.op = Opcode::GAddr;
    ptr.type = Type(Type::Kind::Ptr);
    ptr.operands.push_back(Value::global("counter"));
    ptr.loc = {1, 1, 1};
    entry.instructions.push_back(ptr);

    Instr before;
    before.result = 1;
    before.op = Opcode::Load;
    before.type = Type(Type::Kind::I64);
    before.operands.push_back(Value::temp(0));
    before.loc = {1, 1, 1};
    entry.instructions.push_back(before);

    Instr next;
    next.result = 2;
    next.op = Opcode::Add;
    next.type = Type(Type::Kind::I64);
    next.operands.push_back(Value::temp(1));
    next.operands.push_back(Value::constInt(1));
    next.loc = {1, 1, 1};
    entry.instructions.push_back(next);

    Instr store;
    store.op = Opcode::Store;
    store.type = Type(Type::Kind::I64);
    store.operands.push_back(Value::temp(0));
    store.operands.push_back(Value::temp(2));
    store.loc = {1, 1, 1};
    entry.instructions.push_back(store);

    Instr after;
    after.result = 3;
    after.op = Opcode::Load;
    after.type = Type(Type::Kind::I64);
    after.operands.push_back(Value::temp(0));
    after.loc = {1, 1, 1};
    entry.instructions.push_back(after);

    Instr ret;
    ret.op = Opcode::Ret;
    ret.type = Type(Type::Kind::Void);
    ret.operands.push_back(Value::temp(3));
    ret.loc = {1, 1, 1};
    entry.instructions.push_back(ret);
    entry.terminated = true;

    fn.blocks.push_back(std::move(entry));
    m.functions.push_back(std::move(fn));
    return m;
}

static void test_global_address_storage() {
    std::cout << "  test_global_address_storage: ";

    BytecodeModule bcModule = compileAssumingVerified(createGlobalAddressModule());
    assert(bcModule.globals.size() == 1);
    assert(bcModule.globalIndex["counter"] == 0);

    for (bool threaded : {false, true}) {
        BytecodeVM vm;
        vm.setThreadedDispatch(threaded);
        vm.load(&bcModule);

        BCSlot result = vm.exec("global_addr", {});
        if (vm.state() != VMState::Halted) {
            std::cerr << "global address test trapped (threaded=" << threaded
                      << "): " << vm.trapMessage() << "\n";
        }
        assert(vm.state() == VMState::Halted);
        assert(result.i64 == 42);
    }

    std::cout << "PASSED\n";
}

static void addDirectBytecodeMain(BytecodeModule &module,
                                  std::vector<uint32_t> code,
                                  uint32_t numLocals = 0,
                                  uint32_t maxStack = 4,
                                  bool hasReturn = true) {
    BytecodeFunction fn;
    fn.name = "main";
    fn.numParams = 0;
    fn.numLocals = numLocals;
    fn.maxStack = maxStack;
    fn.hasReturn = hasReturn;
    fn.code = std::move(code);
    module.addFunction(std::move(fn));
}

static BytecodeModule createDirectBytecodeModule(std::vector<uint32_t> code,
                                                 uint32_t numLocals = 0,
                                                 uint32_t maxStack = 4,
                                                 bool hasReturn = true) {
    BytecodeModule module;
    addDirectBytecodeMain(module, std::move(code), numLocals, maxStack, hasReturn);
    return module;
}

static uint16_t addI64(BytecodeModule &module, int64_t value) {
    return static_cast<uint16_t>(module.addI64(value));
}

static uint16_t addF64(BytecodeModule &module, double value) {
    return static_cast<uint16_t>(module.addF64(value));
}

static BCSlot runMain(BytecodeModule &module, bool threaded) {
    BytecodeVM vm;
    vm.setThreadedDispatch(threaded);
    vm.load(&module);
    BCSlot result = vm.exec("main", {});
    if (vm.state() != VMState::Halted) {
        std::cerr << "direct bytecode case trapped (threaded=" << threaded
                  << "): " << vm.trapMessage() << "\n";
    }
    assert(vm.state() == VMState::Halted);
    return result;
}

static void expectMainTrap(BytecodeModule &module, bool threaded, TrapKind expected) {
    BytecodeVM vm;
    vm.setThreadedDispatch(threaded);
    vm.load(&module);
    (void)vm.exec("main", {});
    assert(vm.state() == VMState::Trapped);
    assert(vm.trapKind() == expected);
}

static void test_vm_numeric_edge_regressions() {
    std::cout << "  test_vm_numeric_edge_regressions: ";

    auto binaryI64 = [](BCOpcode op, int64_t lhs, int64_t rhs) {
        BytecodeModule module;
        const uint16_t lhsIdx = addI64(module, lhs);
        const uint16_t rhsIdx = addI64(module, rhs);
        addDirectBytecodeMain(module,
                              {encodeOp16(BCOpcode::LOAD_I64, lhsIdx),
                               encodeOp16(BCOpcode::LOAD_I64, rhsIdx),
                               encodeOp(op),
                               encodeOp(BCOpcode::RETURN)});
        return module;
    };

    const int64_t i64Min = std::numeric_limits<int64_t>::min();
    const int64_t i64Max = std::numeric_limits<int64_t>::max();

    for (bool threaded : {false, true}) {
        {
            BytecodeModule module = binaryI64(BCOpcode::ADD_I64, i64Max, 1);
            assert(runMain(module, threaded).i64 == i64Min);
        }
        {
            BytecodeModule module = binaryI64(BCOpcode::SUB_I64, i64Min, 1);
            assert(runMain(module, threaded).i64 == i64Max);
        }
        {
            BytecodeModule module = binaryI64(BCOpcode::MUL_I64, 1LL << 62, 4);
            assert(runMain(module, threaded).i64 == 0);
        }
        {
            BytecodeModule module = binaryI64(BCOpcode::SHL_I64, -1, 1);
            assert(runMain(module, threaded).i64 == -2);
        }
        {
            BytecodeModule module = binaryI64(BCOpcode::ASHR_I64, -8, 1);
            assert(runMain(module, threaded).i64 == -4);
        }
        {
            BytecodeModule module;
            const uint16_t valueIdx = addI64(module, i64Max);
            addDirectBytecodeMain(module,
                                  {encodeOp16(BCOpcode::LOAD_I64, valueIdx),
                                   encodeOp8(BCOpcode::STORE_LOCAL, 0),
                                   encodeOp8(BCOpcode::INC_LOCAL, 0),
                                   encodeOp8(BCOpcode::LOAD_LOCAL, 0),
                                   encodeOp(BCOpcode::RETURN)},
                                  1,
                                  1);
            assert(runMain(module, threaded).i64 == i64Min);
        }
        {
            BytecodeModule module;
            const uint16_t valueIdx = addI64(module, i64Min);
            addDirectBytecodeMain(module,
                                  {encodeOp16(BCOpcode::LOAD_I64, valueIdx),
                                   encodeOp8(BCOpcode::STORE_LOCAL, 0),
                                   encodeOp8(BCOpcode::DEC_LOCAL, 0),
                                   encodeOp8(BCOpcode::LOAD_LOCAL, 0),
                                   encodeOp(BCOpcode::RETURN)},
                                  1,
                                  1);
            assert(runMain(module, threaded).i64 == i64Max);
        }
        {
            BytecodeModule module;
            const uint16_t lhsIdx = addI64(module, i64Min);
            const uint16_t rhsIdx = addI64(module, -1);
            addDirectBytecodeMain(module,
                                  {encodeOp16(BCOpcode::LOAD_I64, lhsIdx),
                                   encodeOp16(BCOpcode::LOAD_I64, rhsIdx),
                                   encodeOp8(BCOpcode::SREM_I64_CHK, 3),
                                   encodeOp(BCOpcode::RETURN)});
            assert(runMain(module, threaded).i64 == 0);
        }
    }

    std::cout << "PASSED\n";
}

static void test_vm_safety_trap_regressions() {
    std::cout << "  test_vm_safety_trap_regressions: ";

    auto f64TrapModule = [](double value, BCOpcode op) {
        BytecodeModule module;
        const uint16_t valueIdx = addF64(module, value);
        addDirectBytecodeMain(
            module,
            {encodeOp16(BCOpcode::LOAD_F64, valueIdx), encodeOp8(op, 3),
             encodeOp(BCOpcode::RETURN)},
            0,
            1);
        return module;
    };

    for (bool threaded : {false, true}) {
        {
            BytecodeModule module =
                f64TrapModule(std::numeric_limits<double>::quiet_NaN(), BCOpcode::F64_TO_I64);
            expectMainTrap(module, threaded, TrapKind::InvalidCast);
        }
        {
            BytecodeModule module = f64TrapModule(std::ldexp(1.0, 63), BCOpcode::F64_TO_I64_CHK);
            expectMainTrap(module, threaded, TrapKind::Overflow);
        }
        {
            BytecodeModule module = f64TrapModule(std::ldexp(1.0, 64), BCOpcode::F64_TO_U64_CHK);
            expectMainTrap(module, threaded, TrapKind::Overflow);
        }
        {
            BytecodeModule module = createDirectBytecodeModule({encodeOpI8(BCOpcode::LOAD_I8, -1),
                                                                encodeOp(BCOpcode::ALLOCA),
                                                                encodeOp(BCOpcode::RETURN)},
                                                               0,
                                                               1);
            expectMainTrap(module, threaded, TrapKind::DomainError);
        }
        {
            BytecodeModule module = createDirectBytecodeModule(
                {encodeOp8(BCOpcode::LOAD_LOCAL, 3), encodeOp(BCOpcode::RETURN)}, 1, 1);
            expectMainTrap(module, threaded, TrapKind::InvalidOpcode);
        }
        {
            BytecodeModule module = createDirectBytecodeModule({encodeOp(BCOpcode::LOAD_NULL),
                                                                encodeOp(BCOpcode::LOAD_I8_MEM),
                                                                encodeOp(BCOpcode::RETURN)},
                                                               0,
                                                               1);
            expectMainTrap(module, threaded, TrapKind::NullPointer);
        }
        {
            BytecodeModule module = createDirectBytecodeModule({encodeOp(BCOpcode::LOAD_NULL),
                                                                encodeOp(BCOpcode::STR_RETAIN),
                                                                encodeOp(BCOpcode::STR_RELEASE),
                                                                encodeOp(BCOpcode::LOAD_ONE),
                                                                encodeOp(BCOpcode::RETURN)},
                                                               0,
                                                               1);
            assert(runMain(module, threaded).i64 == 1);
        }
    }

    std::cout << "PASSED\n";
}

/// Test basic bytecode encoding/decoding
static void test_bytecode_encoding() {
    std::cout << "  test_bytecode_encoding: ";

    // Test encodeOp8 / decodeArg8_0
    uint32_t instr = encodeOp8(BCOpcode::LOAD_LOCAL, 42);
    assert(decodeOpcode(instr) == BCOpcode::LOAD_LOCAL);
    assert(decodeArg8_0(instr) == 42);

    // Test encodeOp16 / decodeArg16
    instr = encodeOp16(BCOpcode::CALL, 1234);
    assert(decodeOpcode(instr) == BCOpcode::CALL);
    assert(decodeArg16(instr) == 1234);

    // Test encodeOpI16 / decodeArgI16 with negative value
    instr = encodeOpI16(BCOpcode::JUMP, -100);
    assert(decodeOpcode(instr) == BCOpcode::JUMP);
    assert(decodeArgI16(instr) == -100);

    // Test encodeOpI24 / decodeArgI24
    instr = encodeOpI24(BCOpcode::JUMP_LONG, -10000);
    assert(decodeOpcode(instr) == BCOpcode::JUMP_LONG);
    assert(decodeArgI24(instr) == -10000);

    std::cout << "PASSED\n";
}

/// Test basic addition function
static void test_add_function() {
    std::cout << "  test_add_function: ";

    // Create IL module with add function
    Module ilModule = createAddModule();

    // Compile to bytecode
    BytecodeModule bcModule = compileAssumingVerified(ilModule);

    // Verify compilation
    assert(bcModule.functions.size() == 1);
    assert(bcModule.functions[0].name == "add");
    assert(bcModule.functions[0].numParams == 2);

    // Execute
    BytecodeVM vm;
    vm.load(&bcModule);

    BCSlot result = vm.exec("add", {BCSlot::fromInt(3), BCSlot::fromInt(5)});
    assert(vm.state() == VMState::Halted);
    assert(result.i64 == 8);

    // Test with different values
    result = vm.exec("add", {BCSlot::fromInt(100), BCSlot::fromInt(-50)});
    assert(result.i64 == 50);

    std::cout << "PASSED\n";
}

/// Stand-in handler another component might register over a unified one.
static void replacement_thread_start_handler(void **, void *) {}

/// Entering the VM (exec, callbacks, and every class destructor) must not
/// re-install the unified runtime handlers while the extern registry is unchanged
/// — that cost a registry lock, name canonicalization and a signature build per
/// handler on every object death (Legacy Baseball ledger ZB-70) — and must restore
/// them once another component has replaced one.
static void test_unified_handlers_reinstall_only_after_registry_change() {
    std::cout << "  test_unified_handlers_reinstall_only_after_registry_change: ";

    Module ilModule = createAddModule();
    BytecodeModule bcModule = compileAssumingVerified(ilModule);
    BytecodeVM vm;
    vm.load(&bcModule);

    il::vm::ExternRegistry &registry = il::vm::processGlobalExternRegistry();
    const uint64_t settled = il::vm::externRegistryGeneration(registry);
    for (int i = 0; i < 3; ++i) {
        BCSlot result = vm.exec("add", {BCSlot::fromInt(1), BCSlot::fromInt(2)});
        assert(vm.state() == VMState::Halted);
        assert(result.i64 == 3);
    }
    assert(il::vm::externRegistryGeneration(registry) == settled);

    const il::vm::ExternDesc *unified =
        il::vm::RuntimeBridge::findExtern("Zanna.Threads.Thread.Start");
    assert(unified && unified->fn);
    void *const unifiedFn = unified->fn;
    il::vm::ExternDesc replacement = *unified;
    replacement.fn = reinterpret_cast<void *>(&replacement_thread_start_handler);
    il::vm::RuntimeBridge::registerExtern(replacement);

    BCSlot result = vm.exec("add", {BCSlot::fromInt(4), BCSlot::fromInt(5)});
    assert(result.i64 == 9);
    const il::vm::ExternDesc *restored =
        il::vm::RuntimeBridge::findExtern("zanna.threads.thread.start");
    assert(restored && restored->fn == unifiedFn);

    const uint64_t reinstalled = il::vm::externRegistryGeneration(registry);
    result = vm.exec("add", {BCSlot::fromInt(6), BCSlot::fromInt(7)});
    assert(result.i64 == 13);
    assert(il::vm::externRegistryGeneration(registry) == reinstalled);

    std::cout << "PASSED\n";
}

/// Test absolute value function (conditional branching)
static void test_abs_function() {
    std::cout << "  test_abs_function: ";

    // Create IL module with abs function
    Module ilModule = createAbsModule();

    // Compile to bytecode
    BytecodeModule bcModule = compileAssumingVerified(ilModule);

    // Verify compilation
    assert(bcModule.functions.size() == 1);
    assert(bcModule.functions[0].name == "abs");

    // Execute
    BytecodeVM vm;
    vm.load(&bcModule);

    // Test positive
    BCSlot result = vm.exec("abs", {BCSlot::fromInt(5)});
    assert(vm.state() == VMState::Halted);
    assert(result.i64 == 5);

    // Test negative
    result = vm.exec("abs", {BCSlot::fromInt(-10)});
    assert(vm.state() == VMState::Halted);
    assert(result.i64 == 10);

    // Test zero
    result = vm.exec("abs", {BCSlot::fromInt(0)});
    assert(vm.state() == VMState::Halted);
    assert(result.i64 == 0);

    std::cout << "PASSED\n";
}

/// Test fibonacci function (small values)
static void test_fib_small() {
    std::cout << "  test_fib_small: ";

    // Create IL module with fib function
    Module ilModule = createFibModule();

    // Compile to bytecode
    BytecodeModule bcModule = compileAssumingVerified(ilModule);

    // Verify compilation
    assert(bcModule.functions.size() == 1);
    assert(bcModule.functions[0].name == "fib");

    // Execute
    BytecodeVM vm;
    vm.load(&bcModule);

    // Test known fibonacci values
    // fib(0) = 0, fib(1) = 1, fib(2) = 1, fib(3) = 2, fib(4) = 3, fib(5) = 5
    // fib(6) = 8, fib(7) = 13, fib(8) = 21, fib(9) = 34, fib(10) = 55
    int64_t expected[] = {0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 55};

    for (int i = 0; i <= 10; ++i) {
        BCSlot result = vm.exec("fib", {BCSlot::fromInt(i)});
        if (vm.state() != VMState::Halted) {
            std::cerr << "fib(" << i << ") failed with trap: " << vm.trapMessage() << "\n";
            assert(false);
        }
        if (result.i64 != expected[i]) {
            std::cerr << "fib(" << i << ") = " << result.i64 << ", expected " << expected[i]
                      << "\n";
            assert(false);
        }
    }

    std::cout << "PASSED\n";
}

/// Benchmark fibonacci function
static void test_fib_benchmark() {
    std::cout << "  test_fib_benchmark: ";

    // Create IL module with fib function
    Module ilModule = createFibModule();

    // Compile to bytecode
    BytecodeModule bcModule = compileAssumingVerified(ilModule);

    // Execute
    BytecodeVM vm;
    vm.load(&bcModule);

    // Benchmark fib(20)
    auto start = std::chrono::high_resolution_clock::now();
    BCSlot result = vm.exec("fib", {BCSlot::fromInt(20)});
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    assert(vm.state() == VMState::Halted);
    assert(result.i64 == 6765); // fib(20) = 6765

    std::cout << "fib(20)=" << result.i64 << " in " << duration.count() << "ms"
              << " (" << vm.instrCount() << " instructions)"
              << " PASSED\n";
}

/// Test native function calls
/// Creates a function that calls a native "square" function
static void test_native_calls() {
    std::cout << "  test_native_calls: ";

    // Build bytecode module manually
    BytecodeModule bcModule;
    bcModule.magic = kBytecodeModuleMagic;
    bcModule.version = kBytecodeVersion;
    bcModule.flags = 0;

    // Add native function reference: square(i64) -> i64
    uint32_t nativeIdx = bcModule.addNativeFunc("square", 1, true);
    assert(nativeIdx == 0);

    // Create a function that calls the native function:
    // func @call_square(i64 %n) -> i64
    //   %result = call_native @square(%n)
    //   ret %result
    BytecodeFunction func;
    func.name = "call_square";
    func.numParams = 1;
    func.numLocals = 2; // 1 param + 1 result
    func.maxStack = 2;
    func.hasReturn = true;

    // LOAD_LOCAL 0       ; push %n
    func.code.push_back(encodeOp8(BCOpcode::LOAD_LOCAL, 0));
    // CALL_NATIVE [argCount=1][nativeIdx=0]
    func.code.push_back(encodeOp8_16(BCOpcode::CALL_NATIVE, 1, 0));
    // STORE_LOCAL 1      ; store result to local[1]
    func.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 1));
    // LOAD_LOCAL 1       ; push result
    func.code.push_back(encodeOp8(BCOpcode::LOAD_LOCAL, 1));
    // RET
    func.code.push_back(encodeOp(BCOpcode::RETURN));

    bcModule.functions.push_back(std::move(func));
    bcModule.functionIndex["call_square"] = 0;

    // Create VM and register native handler
    BytecodeVM vm;
    vm.registerNativeHandler("square", [](BCSlot *args, uint32_t argCount, BCSlot *result) {
        assert(argCount == 1);
        int64_t n = args[0].i64;
        result->i64 = n * n;
    });

    vm.load(&bcModule);

    // Test with several values
    BCSlot result = vm.exec("call_square", {BCSlot::fromInt(5)});
    assert(vm.state() == VMState::Halted);
    assert(result.i64 == 25); // 5^2 = 25

    result = vm.exec("call_square", {BCSlot::fromInt(10)});
    assert(result.i64 == 100); // 10^2 = 100

    result = vm.exec("call_square", {BCSlot::fromInt(-7)});
    assert(result.i64 == 49); // (-7)^2 = 49

    std::cout << "PASSED\n";
}

/// Test that runtime bridge string calls do not consume aliased bytecode locals.
static void test_runtime_bridge_string_aliasing() {
    std::cout << "  test_runtime_bridge_string_aliasing: ";

    BytecodeModule bcModule;
    bcModule.magic = kBytecodeModuleMagic;
    bcModule.version = kBytecodeVersion;
    bcModule.flags = 0;

    uint32_t baseIdx = bcModule.addString("Whiskers");
    uint32_t infixIdx = bcModule.addString(" says ");
    uint32_t bangIdx = bcModule.addString("!");
    uint32_t concatIdx = bcModule.addNativeFunc("Zanna.String.Concat", 2, true);
    assert(concatIdx == 0);

    BytecodeFunction func;
    func.name = "concat_alias";
    func.numParams = 0;
    func.numLocals = 3;
    func.maxStack = 2;
    func.hasReturn = true;
    func.localIsString = {1, 1, 1};

    // local0 = "Whiskers"
    func.code.push_back(encodeOp16(BCOpcode::LOAD_STR, static_cast<uint16_t>(baseIdx)));
    func.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 0));
    // local1 = local0 + " says "
    func.code.push_back(encodeOp8(BCOpcode::LOAD_LOCAL, 0));
    func.code.push_back(encodeOp16(BCOpcode::LOAD_STR, static_cast<uint16_t>(infixIdx)));
    func.code.push_back(encodeOp8_16(BCOpcode::CALL_NATIVE, 2, static_cast<uint16_t>(concatIdx)));
    func.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 1));
    // local2 = local0 + "!"
    func.code.push_back(encodeOp8(BCOpcode::LOAD_LOCAL, 0));
    func.code.push_back(encodeOp16(BCOpcode::LOAD_STR, static_cast<uint16_t>(bangIdx)));
    func.code.push_back(encodeOp8_16(BCOpcode::CALL_NATIVE, 2, static_cast<uint16_t>(concatIdx)));
    func.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 2));
    // return local2
    func.code.push_back(encodeOp8(BCOpcode::LOAD_LOCAL, 2));
    func.code.push_back(encodeOp(BCOpcode::RETURN));

    bcModule.functions.push_back(std::move(func));
    bcModule.functionIndex["concat_alias"] = 0;

    for (bool threaded : {false, true}) {
        BytecodeVM vm;
        vm.setRuntimeBridgeEnabled(true);
        vm.setThreadedDispatch(threaded);
        vm.load(&bcModule);

        BCSlot result = vm.exec("concat_alias", {});
        assert(vm.state() == VMState::Halted);
        assert(result.ptr != nullptr);
        assert(std::strcmp(rt_string_cstr(static_cast<rt_string>(result.ptr)), "Whiskers!") == 0);
    }

    std::cout << "PASSED\n";
}

/// Test that bytecode memory loads/stores preserve string ownership correctly.
static void test_string_memory_lifetime() {
    std::cout << "  test_string_memory_lifetime: ";

    Module ilModule = createStringFieldLifetimeModule();
    BytecodeModule bcModule = compileAssumingVerified(ilModule);

    for (bool threaded : {false, true}) {
        BytecodeVM vm;
        vm.setRuntimeBridgeEnabled(true);
        vm.setThreadedDispatch(threaded);
        vm.load(&bcModule);

        BCSlot result = vm.exec("field_lifetime", {});
        assert(vm.state() == VMState::Halted);
        assert(result.i64 == 1);
    }

    std::cout << "PASSED\n";
}

/// Test that native string-release helpers consume the argument exactly once.
static void test_string_release_call_lifetime() {
    std::cout << "  test_string_release_call_lifetime: ";

    Module m;
    IRBuilder b(m);

    b.addExtern("Zanna.String.Concat",
                Type(Type::Kind::Str),
                {Type(Type::Kind::Str), Type(Type::Kind::Str)});
    b.addExtern("rt_str_release_maybe", Type(Type::Kind::Void), {Type(Type::Kind::Str)});
    b.addExtern("Zanna.String.Equals",
                Type(Type::Kind::I1),
                {Type(Type::Kind::Str), Type(Type::Kind::Str)});

    auto &fn = b.startFunction("release_roundtrip", Type(Type::Kind::I1), {});
    auto &entry = b.addBlock(fn, "entry");
    b.setInsertPoint(entry);

    Instr slot;
    slot.result = b.reserveTempId();
    slot.op = Opcode::Alloca;
    slot.type = Type(Type::Kind::Ptr);
    slot.operands.push_back(Value::constInt(8));
    slot.loc = {1, 1, 1};
    entry.instructions.push_back(slot);

    Instr left;
    left.result = b.reserveTempId();
    left.op = Opcode::ConstStr;
    left.type = Type(Type::Kind::Str);
    left.operands.push_back(Value::constStr("Ow"));
    left.loc = {1, 1, 1};
    entry.instructions.push_back(left);

    Instr right;
    right.result = b.reserveTempId();
    right.op = Opcode::ConstStr;
    right.type = Type(Type::Kind::Str);
    right.operands.push_back(Value::constStr("ned"));
    right.loc = {1, 1, 1};
    entry.instructions.push_back(right);

    Instr joined;
    joined.result = b.reserveTempId();
    joined.op = Opcode::Call;
    joined.type = Type(Type::Kind::Str);
    joined.callee = "Zanna.String.Concat";
    joined.operands.push_back(Value::temp(*left.result));
    joined.operands.push_back(Value::temp(*right.result));
    joined.loc = {1, 1, 1};
    entry.instructions.push_back(joined);

    Instr storeJoined;
    storeJoined.op = Opcode::Store;
    storeJoined.type = Type(Type::Kind::Str);
    storeJoined.operands.push_back(Value::temp(*slot.result));
    storeJoined.operands.push_back(Value::temp(*joined.result));
    storeJoined.loc = {1, 1, 1};
    entry.instructions.push_back(storeJoined);

    Instr loaded;
    loaded.result = b.reserveTempId();
    loaded.op = Opcode::Load;
    loaded.type = Type(Type::Kind::Str);
    loaded.operands.push_back(Value::temp(*slot.result));
    loaded.loc = {1, 1, 1};
    entry.instructions.push_back(loaded);

    Instr releaseLoaded;
    releaseLoaded.op = Opcode::Call;
    releaseLoaded.type = Type(Type::Kind::Void);
    releaseLoaded.callee = "rt_str_release_maybe";
    releaseLoaded.operands.push_back(Value::temp(*loaded.result));
    releaseLoaded.loc = {1, 1, 1};
    entry.instructions.push_back(releaseLoaded);

    Instr reloaded;
    reloaded.result = b.reserveTempId();
    reloaded.op = Opcode::Load;
    reloaded.type = Type(Type::Kind::Str);
    reloaded.operands.push_back(Value::temp(*slot.result));
    reloaded.loc = {1, 1, 1};
    entry.instructions.push_back(reloaded);

    Instr expected;
    expected.result = b.reserveTempId();
    expected.op = Opcode::ConstStr;
    expected.type = Type(Type::Kind::Str);
    expected.operands.push_back(Value::constStr("Owned"));
    expected.loc = {1, 1, 1};
    entry.instructions.push_back(expected);

    Instr eq;
    eq.result = b.reserveTempId();
    eq.op = Opcode::Call;
    eq.type = Type(Type::Kind::I1);
    eq.callee = "Zanna.String.Equals";
    eq.operands.push_back(Value::temp(*reloaded.result));
    eq.operands.push_back(Value::temp(*expected.result));
    eq.loc = {1, 1, 1};
    entry.instructions.push_back(eq);

    Instr ret;
    ret.op = Opcode::Ret;
    ret.type = Type(Type::Kind::Void);
    ret.operands.push_back(Value::temp(*eq.result));
    ret.loc = {1, 1, 1};
    entry.instructions.push_back(ret);

    BytecodeModule bcModule = compileAssumingVerified(m);

    for (bool threaded : {false, true}) {
        BytecodeVM vm;
        vm.setRuntimeBridgeEnabled(true);
        vm.setThreadedDispatch(threaded);
        vm.load(&bcModule);

        BCSlot result = vm.exec("release_roundtrip", {});
        assert(vm.state() == VMState::Halted);
        assert(result.i64 == 1);
    }

    std::cout << "PASSED\n";
}

/// Test that owned string stack shuffles retain and release exactly once.
static void test_string_stack_op_lifetime() {
    std::cout << "  test_string_stack_op_lifetime: ";

    BytecodeModule bcModule = createStringStackOpsModule();
    for (bool threaded : {false, true}) {
        for (const char *functionName : {"dup_string", "dup2_string", "swap_string", "rot3_string"}) {
            BytecodeVM vm;
            vm.setRuntimeBridgeEnabled(true);
            vm.setThreadedDispatch(threaded);
            vm.load(&bcModule);

            BCSlot result = vm.exec(functionName, {});
            assert(vm.state() == VMState::Halted);
            assert(result.i64 == 1);
        }
    }

    std::cout << "PASSED\n";
}

/// Benchmark comparing switch vs threaded dispatch
static void test_dispatch_benchmark() {
    std::cout << "  test_dispatch_benchmark:\n";

    // Create IL module with fib function
    Module ilModule = createFibModule();

    // Compile to bytecode
    BytecodeModule bcModule = compileAssumingVerified(ilModule);

    // Benchmark with threaded dispatch (default)
    {
        BytecodeVM vm;
        vm.setThreadedDispatch(true);
        vm.load(&bcModule);

        auto start = std::chrono::high_resolution_clock::now();
        BCSlot result = vm.exec("fib", {BCSlot::fromInt(25)});
        auto end = std::chrono::high_resolution_clock::now();

        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        assert(vm.state() == VMState::Halted);
        assert(result.i64 == 75025); // fib(25) = 75025

        std::cout << "    threaded: fib(25)=" << result.i64 << " in " << duration.count() << "us"
                  << " (" << vm.instrCount() << " instrs)\n";
    }

    // Benchmark with switch dispatch
    {
        BytecodeVM vm;
        vm.setThreadedDispatch(false);
        vm.load(&bcModule);

        auto start = std::chrono::high_resolution_clock::now();
        BCSlot result = vm.exec("fib", {BCSlot::fromInt(25)});
        auto end = std::chrono::high_resolution_clock::now();

        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        assert(vm.state() == VMState::Halted);
        assert(result.i64 == 75025); // fib(25) = 75025

        std::cout << "    switch:   fib(25)=" << result.i64 << " in " << duration.count() << "us"
                  << " (" << vm.instrCount() << " instrs)\n";
    }

    std::cout << "    PASSED\n";
}

/// Test a native function that takes multiple arguments
static void test_native_multi_args() {
    std::cout << "  test_native_multi_args: ";

    BytecodeModule bcModule;
    bcModule.magic = kBytecodeModuleMagic;
    bcModule.version = kBytecodeVersion;
    bcModule.flags = 0;

    // Add native function: add3(i64, i64, i64) -> i64
    uint32_t nativeIdx = bcModule.addNativeFunc("add3", 3, true);
    assert(nativeIdx == 0);

    // func @call_add3(i64 %a, i64 %b, i64 %c) -> i64
    //   %result = call_native @add3(%a, %b, %c)
    //   ret %result
    BytecodeFunction func;
    func.name = "call_add3";
    func.numParams = 3;
    func.numLocals = 4; // 3 params + 1 result
    func.maxStack = 4;
    func.hasReturn = true;

    // Push args in order
    func.code.push_back(encodeOp8(BCOpcode::LOAD_LOCAL, 0)); // push %a
    func.code.push_back(encodeOp8(BCOpcode::LOAD_LOCAL, 1)); // push %b
    func.code.push_back(encodeOp8(BCOpcode::LOAD_LOCAL, 2)); // push %c
    // CALL_NATIVE [argCount=3][nativeIdx=0]
    func.code.push_back(encodeOp8_16(BCOpcode::CALL_NATIVE, 3, 0));
    // RET (result is on stack)
    func.code.push_back(encodeOp(BCOpcode::RETURN));

    bcModule.functions.push_back(std::move(func));
    bcModule.functionIndex["call_add3"] = 0;

    BytecodeVM vm;
    vm.registerNativeHandler("add3", [](BCSlot *args, uint32_t argCount, BCSlot *result) {
        assert(argCount == 3);
        result->i64 = args[0].i64 + args[1].i64 + args[2].i64;
    });

    vm.load(&bcModule);

    BCSlot result =
        vm.exec("call_add3", {BCSlot::fromInt(10), BCSlot::fromInt(20), BCSlot::fromInt(30)});
    assert(vm.state() == VMState::Halted);
    assert(result.i64 == 60); // 10 + 20 + 30

    result = vm.exec("call_add3", {BCSlot::fromInt(1), BCSlot::fromInt(2), BCSlot::fromInt(3)});
    assert(result.i64 == 6);

    std::cout << "PASSED\n";
}

/// Test native function indices beyond 255 through the compiler and VM.
static void test_native_wide_index() {
    std::cout << "  test_native_wide_index: ";

    Module ilModule = createWideNativeIndexModule();

    BytecodeModule bcModule = compileAssumingVerified(ilModule);

    assert(bcModule.nativeFuncs.size() == 257);

    const BytecodeFunction *func = bcModule.findFunction("call_wide_native");
    assert(func != nullptr);

    std::vector<uint32_t> nativeCalls;
    for (uint32_t instr : func->code) {
        if (decodeOpcode(instr) == BCOpcode::CALL_NATIVE) {
            nativeCalls.push_back(instr);
        }
    }

    assert(nativeCalls.size() == 257);
    assert(decodeArg8_0(nativeCalls.back()) == 0);
    assert(decodeArg16_1(nativeCalls.back()) == 256);

    BytecodeVM vm;
    for (uint32_t i = 0; i <= 256; ++i) {
        vm.registerNativeHandler("native_" + std::to_string(i),
                                 [i](BCSlot *args, uint32_t argCount, BCSlot *result) {
                                     assert(argCount == 0);
                                     (void)args;
                                     result->i64 = static_cast<int64_t>(i);
                                 });
    }

    vm.load(&bcModule);

    BCSlot result = vm.exec("call_wide_native", {});
    assert(vm.state() == VMState::Halted);
    assert(result.i64 == 256);

    std::cout << "PASSED\n";
}

/// Test exception handling with EH_PUSH, TRAP, and handler dispatch
static void test_exception_handling() {
    std::cout << "  test_exception_handling: ";
#ifdef _WIN32
    // Skip on Windows: bytecode VM exception handling has issues (to be investigated)
    std::cout << "SKIPPED (Windows)\n";
    return;
#endif

    BytecodeModule bcModule;
    bcModule.magic = kBytecodeModuleMagic;
    bcModule.version = kBytecodeVersion;
    bcModule.flags = 0;

    // Create a function that:
    // 1. Pushes an exception handler
    // 2. Raises a trap
    // 3. Handler catches it and returns a sentinel value
    //
    // func @test_trap() -> i64
    //   eh_push handler  ; offset to handler
    //   trap 7           ; RuntimeError = 7
    //   load_i64 999     ; unreachable
    //   ret
    // handler:
    //   eh_entry
    //   pop              ; discard trap kind from stack
    //   load_i64 42      ; handler returns 42
    //   ret

    BytecodeFunction func;
    func.name = "test_trap";
    func.numParams = 0;
    func.numLocals = 2; // Need 2 locals for handler params
    func.maxStack = 4;
    func.hasReturn = true;

    // eh_push with raw offset to handler in next word
    // Layout: EH_PUSH at pc=0, offset at pc=1, TRAP at pc=2, LOAD_I8 at pc=3, RETURN at pc=4
    // Handler will be at pc=5
    // Offset = handler_pc - offset_word_pc = 5 - 1 = 4
    func.code.push_back(encodeOp(BCOpcode::EH_PUSH));
    func.code.push_back(static_cast<uint32_t>(4)); // Raw offset to handler
    // trap RuntimeError
    func.code.push_back(encodeOp8(BCOpcode::TRAP, static_cast<uint8_t>(TrapKind::RuntimeError)));
    // These should be unreachable:
    func.code.push_back(encodeOp8(BCOpcode::LOAD_I8, static_cast<uint8_t>(999 & 0xFF)));
    func.code.push_back(encodeOp(BCOpcode::RETURN));
    // handler: (pc = 6)
    // dispatchTrap pushes trap kind and resume token, handler stores them to locals
    func.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 1)); // Store resume token to local 1
    func.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 0)); // Store trap kind to local 0
    func.code.push_back(encodeOp(BCOpcode::EH_ENTRY));
    // Load 42 as the return value
    func.code.push_back(encodeOp8(BCOpcode::LOAD_I8, 42));
    func.code.push_back(encodeOp(BCOpcode::RETURN));

    bcModule.functions.push_back(std::move(func));
    bcModule.functionIndex["test_trap"] = 0;

    BytecodeVM vm;
    vm.load(&bcModule);

    BCSlot result = vm.exec("test_trap", {});
    assert(vm.state() == VMState::Halted);
    assert(result.i64 == 42); // Handler returned 42

    std::cout << "PASSED\n";
}

/// Test unhandled trap
static void test_unhandled_trap() {
    std::cout << "  test_unhandled_trap: ";

    BytecodeModule bcModule;
    bcModule.magic = kBytecodeModuleMagic;
    bcModule.version = kBytecodeVersion;
    bcModule.flags = 0;

    // Function that raises a trap with no handler
    // func @unhandled() -> i64
    //   trap 7  ; RuntimeError
    //   ret     ; unreachable

    BytecodeFunction func;
    func.name = "unhandled";
    func.numParams = 0;
    func.numLocals = 1;
    func.maxStack = 2;

    func.code.push_back(encodeOp8(BCOpcode::TRAP, static_cast<uint8_t>(TrapKind::RuntimeError)));
    func.code.push_back(encodeOp8(BCOpcode::LOAD_I8, 0));
    func.code.push_back(encodeOp(BCOpcode::RETURN));
    func.lineTable = {42, 43, 44};
    func.sourceFileTable = {1, 1, 1};
    func.blockLabelTable = {"entry", "entry", "entry"};

    bcModule.sourceFiles.push_back({"unhandled_trap.zia", 0});
    bcModule.functions.push_back(std::move(func));
    bcModule.functionIndex["unhandled"] = 0;

    BytecodeVM vm;
    vm.load(&bcModule);

    vm.exec("unhandled", {});
    assert(vm.state() == VMState::Trapped);
    assert(vm.trapKind() == TrapKind::RuntimeError);
    const std::string trapMessage = vm.trapMessage();
    assert(trapMessage.find("Trap @unhandled:entry#0") != std::string::npos);
    assert(trapMessage.find("unhandled_trap.zia:42") != std::string::npos);
    assert(trapMessage.find("RuntimeError") != std::string::npos);

    std::cout << "PASSED\n";
}

/// Test TrapFromErr maps legacy error numbers to structured trap kinds.
static void test_trap_from_err_maps_legacy_error_code() {
    std::cout << "  test_trap_from_err_maps_legacy_error_code: ";

    BytecodeModule bcModule;
    bcModule.magic = kBytecodeModuleMagic;
    bcModule.version = kBytecodeVersion;
    bcModule.flags = 0;

    BytecodeFunction func;
    func.name = "trap_from_err";
    func.numParams = 0;
    func.numLocals = 0;
    func.maxStack = 1;

    func.code.push_back(encodeOp8(BCOpcode::LOAD_I8, 7));
    func.code.push_back(encodeOp(BCOpcode::TRAP_FROM_ERR));
    func.code.push_back(encodeOp(BCOpcode::RETURN));
    func.lineTable = {31, 32, 33};
    func.sourceFileTable = {1, 1, 1};
    func.blockLabelTable = {"entry", "entry", "entry"};

    bcModule.sourceFiles.push_back({"trap_from_err.zia", 0});
    bcModule.functions.push_back(std::move(func));
    bcModule.functionIndex["trap_from_err"] = 0;

    for (bool threaded : {false, true}) {
        BytecodeVM vm;
        vm.setThreadedDispatch(threaded);
        vm.load(&bcModule);

        vm.exec("trap_from_err", {});
        assert(vm.state() == VMState::Trapped);
        assert(vm.trapKind() == TrapKind::Bounds);
        assert(vm.trapMessage().find("Bounds (code=7)") != std::string::npos);
    }

    std::cout << "PASSED\n";
}

/// Test EH_POP (handler unregistration)
static void test_eh_pop() {
    std::cout << "  test_eh_pop: ";

    BytecodeModule bcModule;
    bcModule.magic = kBytecodeModuleMagic;
    bcModule.version = kBytecodeVersion;
    bcModule.flags = 0;

    // Function that registers and unregisters a handler, then traps
    // func @test_eh_pop() -> i64
    //   eh_push handler
    //   eh_pop           ; unregister handler
    //   trap 7           ; should be unhandled now
    //   ret
    // handler:
    //   eh_entry
    //   load_i64 42
    //   ret

    BytecodeFunction func;
    func.name = "test_eh_pop";
    func.numParams = 0;
    func.numLocals = 2;
    func.maxStack = 4;

    // Layout: EH_PUSH at pc=0, offset at pc=1, EH_POP at pc=2, TRAP at pc=3, RETURN at pc=4
    // Handler at pc=5
    // Offset = 5 - 1 = 4
    func.code.push_back(encodeOp(BCOpcode::EH_PUSH));
    func.code.push_back(static_cast<uint32_t>(4));   // Raw offset to handler
    func.code.push_back(encodeOp(BCOpcode::EH_POP)); // unregister
    func.code.push_back(encodeOp8(
        BCOpcode::TRAP, static_cast<uint8_t>(TrapKind::RuntimeError))); // should be unhandled
    func.code.push_back(encodeOp(BCOpcode::RETURN));
    // handler (unreachable):
    func.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 1)); // Store resume token
    func.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 0)); // Store trap kind
    func.code.push_back(encodeOp(BCOpcode::EH_ENTRY));
    func.code.push_back(encodeOp8(BCOpcode::LOAD_I8, 42));
    func.code.push_back(encodeOp(BCOpcode::RETURN));

    bcModule.functions.push_back(std::move(func));
    bcModule.functionIndex["test_eh_pop"] = 0;

    BytecodeVM vm;
    vm.load(&bcModule);

    vm.exec("test_eh_pop", {});
    assert(vm.state() == VMState::Trapped); // Should be unhandled
    assert(vm.trapKind() == TrapKind::RuntimeError);

    std::cout << "PASSED\n";
}

/// Test that runtime traps from CALL_NATIVE route into bytecode handlers.
static void test_runtime_bridge_trap_dispatch() {
    std::cout << "  test_runtime_bridge_trap_dispatch: ";

    BytecodeModule bcModule;
    bcModule.magic = kBytecodeModuleMagic;
    bcModule.version = kBytecodeVersion;
    bcModule.flags = 0;

    const uint16_t trapIdx =
        static_cast<uint16_t>(bcModule.addNativeFunc("Zanna.Core.Diagnostics.Trap", 1, false));
    bcModule.stringPool.push_back("boom");

    BytecodeFunction func;
    func.name = "native_trap";
    func.numParams = 0;
    func.numLocals = 2;
    func.maxStack = 4;
    func.hasReturn = true;
    func.code.push_back(encodeOp(BCOpcode::EH_PUSH));
    func.code.push_back(static_cast<uint32_t>(6)); // handler at pc 7, offset word at pc 1
    func.code.push_back(encodeOp16(BCOpcode::LOAD_STR, 0));
    func.code.push_back(encodeOp8_16(BCOpcode::CALL_NATIVE, 1, trapIdx));
    func.code.push_back(encodeOp8(BCOpcode::LOAD_I8, 99));
    func.code.push_back(encodeOp(BCOpcode::RETURN));
    func.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 1));
    func.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 0));
    func.code.push_back(encodeOp(BCOpcode::EH_ENTRY));
    func.code.push_back(encodeOp8(BCOpcode::LOAD_I8, 55));
    func.code.push_back(encodeOp(BCOpcode::RETURN));
    func.lineTable.assign(func.code.size(), 0);
    func.lineTable[3] = 77;

    bcModule.functions.push_back(std::move(func));
    bcModule.functionIndex["native_trap"] = 0;

    for (bool threaded : {false, true}) {
        BytecodeVM vm;
        vm.setRuntimeBridgeEnabled(true);
        vm.setThreadedDispatch(threaded);
        vm.load(&bcModule);

        BCSlot result = vm.exec("native_trap", {});
        assert(vm.state() == VMState::Halted);
        assert(result.i64 == 55);
    }

    std::cout << "PASSED\n";
}

/// Test resume.next and trap metadata after a checked arithmetic trap.
static void test_resume_next_and_trap_metadata() {
    std::cout << "  test_resume_next_and_trap_metadata: ";

    BytecodeModule bcModule;
    bcModule.magic = kBytecodeModuleMagic;
    bcModule.version = kBytecodeVersion;
    bcModule.flags = 0;

    BytecodeFunction resumeFunc;
    resumeFunc.name = "resume_next";
    resumeFunc.numParams = 0;
    resumeFunc.numLocals = 2;
    resumeFunc.maxStack = 4;
    resumeFunc.hasReturn = true;
    resumeFunc.code.push_back(encodeOp(BCOpcode::EH_PUSH));
    resumeFunc.code.push_back(static_cast<uint32_t>(6)); // handler at pc 7
    resumeFunc.code.push_back(encodeOp8(BCOpcode::LOAD_I8, 10));
    resumeFunc.code.push_back(encodeOp(BCOpcode::LOAD_ZERO));
    resumeFunc.code.push_back(encodeOp8(BCOpcode::SDIV_I64_CHK, 3));
    resumeFunc.code.push_back(encodeOp8(BCOpcode::LOAD_I8, 7));
    resumeFunc.code.push_back(encodeOp(BCOpcode::RETURN));
    resumeFunc.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 1));
    resumeFunc.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 0));
    resumeFunc.code.push_back(encodeOp(BCOpcode::EH_ENTRY));
    resumeFunc.code.push_back(encodeOp8(BCOpcode::LOAD_LOCAL, 1));
    resumeFunc.code.push_back(encodeOp(BCOpcode::RESUME_NEXT));
    resumeFunc.lineTable.assign(resumeFunc.code.size(), 0);
    resumeFunc.lineTable[4] = 123;

    BytecodeFunction metaFunc;
    metaFunc.name = "trap_meta";
    metaFunc.numParams = 0;
    metaFunc.numLocals = 2;
    metaFunc.maxStack = 4;
    metaFunc.hasReturn = true;
    metaFunc.code.push_back(encodeOp(BCOpcode::EH_PUSH));
    metaFunc.code.push_back(static_cast<uint32_t>(6)); // handler at pc 7
    metaFunc.code.push_back(encodeOp8(BCOpcode::LOAD_I8, 10));
    metaFunc.code.push_back(encodeOp(BCOpcode::LOAD_ZERO));
    metaFunc.code.push_back(encodeOp8(BCOpcode::SDIV_I64_CHK, 3));
    metaFunc.code.push_back(encodeOp8(BCOpcode::LOAD_I8, 0));
    metaFunc.code.push_back(encodeOp(BCOpcode::RETURN));
    metaFunc.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 1));
    metaFunc.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 0));
    metaFunc.code.push_back(encodeOp(BCOpcode::EH_ENTRY));
    metaFunc.code.push_back(encodeOp8(BCOpcode::LOAD_LOCAL, 0));
    metaFunc.code.push_back(encodeOp(BCOpcode::ERR_GET_IP));
    metaFunc.code.push_back(encodeOp8(BCOpcode::LOAD_LOCAL, 0));
    metaFunc.code.push_back(encodeOp(BCOpcode::ERR_GET_LINE));
    metaFunc.code.push_back(encodeOpI16(BCOpcode::LOAD_I16, 10));
    metaFunc.code.push_back(encodeOp(BCOpcode::MUL_I64));
    metaFunc.code.push_back(encodeOp(BCOpcode::ADD_I64));
    metaFunc.code.push_back(encodeOp(BCOpcode::RETURN));
    metaFunc.lineTable.assign(metaFunc.code.size(), 0);
    metaFunc.lineTable[4] = 123;

    bcModule.functions.push_back(std::move(resumeFunc));
    bcModule.functionIndex["resume_next"] = 0;
    bcModule.functions.push_back(std::move(metaFunc));
    bcModule.functionIndex["trap_meta"] = 1;

    for (bool threaded : {false, true}) {
        BytecodeVM vm;
        vm.setThreadedDispatch(threaded);
        vm.load(&bcModule);

        BCSlot resumed = vm.exec("resume_next", {});
        assert(vm.state() == VMState::Halted);
        assert(resumed.i64 == 7);

        BCSlot meta = vm.exec("trap_meta", {});
        assert(vm.state() == VMState::Halted);
        assert(meta.i64 == 1234); // fault pc 4, source line 123
    }

    std::cout << "PASSED\n";
}

/// Test resume.label consumes its resume token and leaves the operand stack balanced.
static void test_resume_label_consumes_token() {
    std::cout << "  test_resume_label_consumes_token: ";

    BytecodeModule bcModule;
    bcModule.magic = kBytecodeModuleMagic;
    bcModule.version = kBytecodeVersion;
    bcModule.flags = 0;

    BytecodeFunction fn;
    fn.name = "resume_label";
    fn.numParams = 0;
    fn.numLocals = 3;
    fn.maxStack = 2;
    fn.hasReturn = true;
    fn.code.push_back(encodeOp(BCOpcode::LOAD_ZERO));
    fn.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 0));
    fn.code.push_back(encodeOp(BCOpcode::EH_PUSH));
    fn.code.push_back(static_cast<uint32_t>(9)); // handler at pc 12
    fn.code.push_back(encodeOp8(BCOpcode::LOAD_I8, 1));
    fn.code.push_back(encodeOp(BCOpcode::LOAD_ZERO));
    fn.code.push_back(encodeOp8(BCOpcode::SDIV_I64_CHK, 3));
    fn.code.push_back(encodeOp(BCOpcode::EH_POP));
    fn.code.push_back(encodeOp8(BCOpcode::LOAD_I8, 99));
    fn.code.push_back(encodeOp(BCOpcode::RETURN));
    fn.code.push_back(encodeOp(BCOpcode::NOP));
    fn.code.push_back(encodeOp(BCOpcode::NOP));
    fn.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 2)); // resume token
    fn.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 1)); // error token
    fn.code.push_back(encodeOp(BCOpcode::EH_ENTRY));
    fn.code.push_back(encodeOp(BCOpcode::LOAD_ONE));
    fn.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 0));
    fn.code.push_back(encodeOp8(BCOpcode::LOAD_LOCAL, 2));
    fn.code.push_back(encodeOp(BCOpcode::RESUME_LABEL));
    fn.code.push_back(static_cast<uint32_t>(2)); // jump to pc 21
    fn.code.push_back(encodeOp(BCOpcode::NOP));
    fn.code.push_back(encodeOp8(BCOpcode::LOAD_LOCAL, 0));
    fn.code.push_back(encodeOp(BCOpcode::RETURN));
    fn.lineTable.assign(fn.code.size(), 0);

    bcModule.functions.push_back(std::move(fn));
    bcModule.functionIndex["resume_label"] = 0;

    for (bool threaded : {false, true}) {
        BytecodeVM vm;
        vm.setThreadedDispatch(threaded);
        vm.load(&bcModule);

        BCSlot result = vm.exec("resume_label", {});
        assert(vm.state() == VMState::Halted);
        assert(result.i64 == 1);
    }

    std::cout << "PASSED\n";
}

/// Test that malformed bytecode metadata traps instead of silently continuing.
static void test_invalid_pool_and_global_indices() {
    std::cout << "  test_invalid_pool_and_global_indices: ";

    BytecodeModule badConstModule;
    badConstModule.magic = kBytecodeModuleMagic;
    badConstModule.version = kBytecodeVersion;
    badConstModule.flags = 0;

    BytecodeFunction badConst;
    badConst.name = "bad_const";
    badConst.numParams = 0;
    badConst.numLocals = 0;
    badConst.maxStack = 1;
    badConst.code.push_back(encodeOp16(BCOpcode::LOAD_I64, 0));
    badConst.code.push_back(encodeOp(BCOpcode::RETURN));
    badConstModule.functions.push_back(std::move(badConst));
    badConstModule.functionIndex["bad_const"] = 0;

    BytecodeModule badGlobalModule;
    badGlobalModule.magic = kBytecodeModuleMagic;
    badGlobalModule.version = kBytecodeVersion;
    badGlobalModule.flags = 0;

    BytecodeFunction badGlobal;
    badGlobal.name = "bad_global";
    badGlobal.numParams = 0;
    badGlobal.numLocals = 0;
    badGlobal.maxStack = 1;
    badGlobal.code.push_back(encodeOp16(BCOpcode::LOAD_GLOBAL, 0));
    badGlobal.code.push_back(encodeOp(BCOpcode::RETURN));
    badGlobalModule.functions.push_back(std::move(badGlobal));
    badGlobalModule.functionIndex["bad_global"] = 0;

    BytecodeModule badFrameModule;
    badFrameModule.magic = kBytecodeModuleMagic;
    badFrameModule.version = kBytecodeVersion;
    badFrameModule.flags = 0;

    BytecodeFunction badFrame;
    badFrame.name = "bad_frame";
    badFrame.numParams = 1;
    badFrame.numLocals = 0;
    badFrame.maxStack = 1;
    badFrame.code.push_back(encodeOp(BCOpcode::RETURN));
    badFrameModule.functions.push_back(std::move(badFrame));
    badFrameModule.functionIndex["bad_frame"] = 0;

    for (bool threaded : {false, true}) {
        {
            BytecodeVM vm;
            vm.setThreadedDispatch(threaded);
            vm.load(&badConstModule);
            (void)vm.exec("bad_const", {});
            assert(vm.state() == VMState::Trapped);
            assert(vm.trapKind() == TrapKind::InvalidOpcode);
        }
        {
            BytecodeVM vm;
            vm.setThreadedDispatch(threaded);
            vm.load(&badGlobalModule);
            (void)vm.exec("bad_global", {});
            assert(vm.state() == VMState::Trapped);
            assert(vm.trapKind() == TrapKind::InvalidOpcode);
        }
        {
            BytecodeVM vm;
            vm.setThreadedDispatch(threaded);
            vm.load(&badFrameModule);
            (void)vm.exec("bad_frame", {BCSlot::fromInt(1)});
            assert(vm.state() == VMState::Trapped);
            assert(vm.trapKind() == TrapKind::InvalidOpcode);
        }
    }

    std::cout << "PASSED\n";
}

/// Test debug API (breakpoints, single-step)
/// Note: Actual breakpoint interception requires debug-enabled execution mode
static void test_debug_api() {
    std::cout << "  test_debug_api: ";

    BytecodeVM vm;

    // Test breakpoint API
    vm.setBreakpoint("test_func", 0);
    vm.setBreakpoint("test_func", 10);
    vm.setBreakpoint("other_func", 5);

    // Clear specific breakpoint
    vm.clearBreakpoint("test_func", 0);

    // Clear all
    vm.clearAllBreakpoints();

    // Test single-step API
    assert(vm.singleStep() == false);
    vm.setSingleStep(true);
    assert(vm.singleStep() == true);
    vm.setSingleStep(false);

    // Test debug callback
    bool callbackCalled = false;
    vm.setDebugCallback([&](BytecodeVM &, const BytecodeFunction *, uint32_t, bool) {
        callbackCalled = true;
        return true;
    });

    // Test getter methods
    assert(vm.currentPc() == 0); // No function running
    assert(vm.currentFunction() == nullptr);
    assert(vm.exceptionHandlerDepth() == 0);

    std::cout << "PASSED\n";
}

/// Test that function entry arity mismatches trap cleanly.
static void test_entry_arity_mismatch() {
    std::cout << "  test_entry_arity_mismatch: ";

    Module ilModule = createAddModule();
    BytecodeModule bcModule = compileAssumingVerified(ilModule);

    for (bool threaded : {false, true}) {
        BytecodeVM vm;
        vm.setThreadedDispatch(threaded);
        vm.load(&bcModule);

        (void)vm.exec("add", {BCSlot::fromInt(7)});
        assert(vm.state() == VMState::Trapped);
        assert(vm.trapKind() == TrapKind::RuntimeError);
    }

    std::cout << "PASSED\n";
}

/// Test that invalid branch targets trap instead of reading past code bounds.
static void test_invalid_branch_target() {
    std::cout << "  test_invalid_branch_target: ";

    BytecodeModule bcModule;
    bcModule.magic = kBytecodeModuleMagic;
    bcModule.version = kBytecodeVersion;
    bcModule.flags = 0;

    BytecodeFunction func;
    func.name = "bad_jump";
    func.numParams = 0;
    func.numLocals = 0;
    func.maxStack = 1;
    func.code.push_back(encodeOpI16(BCOpcode::JUMP, 10));
    func.code.push_back(encodeOp8(BCOpcode::LOAD_I8, 1));
    func.code.push_back(encodeOp(BCOpcode::RETURN));

    bcModule.functions.push_back(std::move(func));
    bcModule.functionIndex["bad_jump"] = 0;

    for (bool threaded : {false, true}) {
        BytecodeVM vm;
        vm.setThreadedDispatch(threaded);
        vm.load(&bcModule);

        (void)vm.exec("bad_jump", {});
        assert(vm.state() == VMState::Trapped);
        assert(vm.trapKind() == TrapKind::InvalidOpcode);
    }

    std::cout << "PASSED\n";
}

/// Test that truncated multi-word instructions trap cleanly.
static void test_truncated_extended_instruction() {
    std::cout << "  test_truncated_extended_instruction: ";

    BytecodeModule bcModule;
    bcModule.magic = kBytecodeModuleMagic;
    bcModule.version = kBytecodeVersion;
    bcModule.flags = 0;

    BytecodeFunction func;
    func.name = "truncated_load_i32";
    func.numParams = 0;
    func.numLocals = 0;
    func.maxStack = 1;
    func.code.push_back(encodeOp(BCOpcode::LOAD_I32));

    bcModule.functions.push_back(std::move(func));
    bcModule.functionIndex["truncated_load_i32"] = 0;

    for (bool threaded : {false, true}) {
        BytecodeVM vm;
        vm.setThreadedDispatch(threaded);
        vm.load(&bcModule);

        (void)vm.exec("truncated_load_i32", {});
        assert(vm.state() == VMState::Trapped);
        assert(vm.trapKind() == TrapKind::InvalidOpcode);
    }

    std::cout << "PASSED\n";
}

static void test_branch_target_rejects_inline_word() {
    std::cout << "  test_branch_target_rejects_inline_word: ";

    BytecodeModule bcModule;
    BytecodeFunction func;
    func.name = "main";
    func.numParams = 0;
    func.numLocals = 0;
    func.maxStack = 1;
    func.hasReturn = true;
    func.code.push_back(encodeOpI16(BCOpcode::JUMP, 1));
    func.code.push_back(encodeOp(BCOpcode::LOAD_I32));
    func.code.push_back(123u);
    func.code.push_back(encodeOp(BCOpcode::RETURN));
    bcModule.addFunction(std::move(func));

    for (bool threaded : {false, true})
        expectMainTrap(bcModule, threaded, TrapKind::InvalidOpcode);

    std::cout << "PASSED\n";
}

static void test_alloca_typed_memory_range_check() {
    std::cout << "  test_alloca_typed_memory_range_check: ";

    BytecodeModule module = createDirectBytecodeModule({encodeOpI8(BCOpcode::LOAD_I8, 1),
                                                        encodeOp(BCOpcode::ALLOCA),
                                                        encodeOpI8(BCOpcode::LOAD_I8, 1),
                                                        encodeOp(BCOpcode::GEP),
                                                        encodeOp8(BCOpcode::LOAD_I8, 42),
                                                        encodeOp(BCOpcode::STORE_I64_MEM),
                                                        encodeOp(BCOpcode::LOAD_ONE),
                                                        encodeOp(BCOpcode::RETURN)},
                                                       0,
                                                       2);

    for (bool threaded : {false, true})
        expectMainTrap(module, threaded, TrapKind::Bounds);

    std::cout << "PASSED\n";
}

static void test_indirect_call_arity_mismatch_traps() {
    std::cout << "  test_indirect_call_arity_mismatch_traps: ";

    BytecodeModule module;

    BytecodeFunction callee;
    callee.name = "callee";
    callee.numParams = 1;
    callee.numLocals = 1;
    callee.maxStack = 1;
    callee.hasReturn = true;
    callee.code.push_back(encodeOp8(BCOpcode::LOAD_LOCAL, 0));
    callee.code.push_back(encodeOp(BCOpcode::RETURN));
    module.addFunction(std::move(callee));

    const uint16_t calleePtr = addI64(module, std::numeric_limits<int64_t>::min());

    BytecodeFunction mainFunc;
    mainFunc.name = "main";
    mainFunc.numParams = 0;
    mainFunc.numLocals = 0;
    mainFunc.maxStack = 1;
    mainFunc.hasReturn = true;
    mainFunc.code.push_back(encodeOp16(BCOpcode::LOAD_I64, calleePtr));
    mainFunc.code.push_back(encodeOp8(BCOpcode::CALL_INDIRECT, 0));
    mainFunc.code.push_back(encodeOp(BCOpcode::RETURN));
    module.addFunction(std::move(mainFunc));

    for (bool threaded : {false, true})
        expectMainTrap(module, threaded, TrapKind::RuntimeError);

    std::cout << "PASSED\n";
}

static void test_trusted_dispatch_toggle() {
    std::cout << "  test_trusted_dispatch_toggle: ";

    BytecodeModule bcModule;
    BytecodeFunction fn;
    fn.name = "main";
    fn.hasReturn = true;
    fn.numParams = 0;
    fn.numLocals = 0;
    fn.maxStack = 1;
    fn.code.push_back(encodeOp(BCOpcode::LOAD_ONE));
    fn.code.push_back(encodeOp(BCOpcode::RETURN));
    bcModule.addFunction(std::move(fn));

    for (bool trusted : {false, true}) {
        BytecodeVM vm;
        vm.setThreadedDispatch(false);
        vm.setTrustedDispatch(trusted);
        vm.load(&bcModule);
        BCSlot result = vm.exec("main", {});
        assert(vm.state() == VMState::Halted);
        assert(result.i64 == 1);
    }

    std::cout << "PASSED\n";
}

static void test_native_refs_cache_runtime_metadata() {
    std::cout << "  test_native_refs_cache_runtime_metadata: ";

    BytecodeModule bcModule;
    uint32_t idx = bcModule.addNativeFunc("rt_abs_i64", 1, true);
    const NativeFuncRef &ref = bcModule.nativeFuncs[idx];
    assert(ref.runtimeDescriptor != nullptr);
    assert(ref.runtimeSignature != nullptr);
    assert(!ref.returnsString);

    std::cout << "PASSED\n";
}

static void test_array_fast_path_bytecode_ops() {
    std::cout << "  test_array_fast_path_bytecode_ops: ";

    for (ArrayFastElementKind kind :
         {ArrayFastElementKind::I32, ArrayFastElementKind::I64, ArrayFastElementKind::F64}) {
        BytecodeModule bcModule = compileAssumingVerified(createArrayFastPathModule(kind));
        assert(bcModule.nativeFuncs.empty());

        const BytecodeFunction *main = bcModule.findFunction("main");
        assert(main != nullptr);
        assertArrayFastOpcodesPresent(*main, kind);

        for (bool threaded : {false, true}) {
            for (bool trusted : {false, true}) {
                BytecodeVM vm;
                vm.setThreadedDispatch(threaded);
                vm.setTrustedDispatch(trusted);
                vm.load(&bcModule);

                if (kind == ArrayFastElementKind::I32) {
                    int32_t storage[2] = {0, 0};
                    BCSlot result = vm.exec("main", {BCSlot::fromPtr(storage)});
                    assert(vm.state() == VMState::Halted);
                    assert(result.i64 == 123);
                    assert(storage[1] == 123);
                } else if (kind == ArrayFastElementKind::I64) {
                    int64_t storage[2] = {0, 0};
                    BCSlot result = vm.exec("main", {BCSlot::fromPtr(storage)});
                    assert(vm.state() == VMState::Halted);
                    assert(result.i64 == 1234567890123LL);
                    assert(storage[1] == 1234567890123LL);
                } else {
                    double storage[2] = {0.0, 0.0};
                    BCSlot result = vm.exec("main", {BCSlot::fromPtr(storage)});
                    assert(vm.state() == VMState::Halted);
                    assert(result.f64 == 123.5);
                    assert(storage[1] == 123.5);
                }
            }
        }

        BytecodeModule overflowModule = createArrayFastOverflowModule(kind);
        for (bool threaded : {false, true}) {
            for (bool trusted : {false, true}) {
                int64_t storage[2] = {0, 0};
                BytecodeVM vm;
                vm.setThreadedDispatch(threaded);
                vm.setTrustedDispatch(trusted);
                vm.load(&overflowModule);
                (void)vm.exec("main", {BCSlot::fromPtr(storage)});
                assert(vm.state() == VMState::Trapped);
                assert(vm.trapKind() == TrapKind::Bounds);
            }
        }
    }

    std::cout << "PASSED\n";
}

/// Test that bytecode Thread.StartSafe records worker traps instead of swallowing them.
static void test_thread_start_safe_reports_bytecode_trap() {
    std::cout << "  test_thread_start_safe_reports_bytecode_trap: ";

    BytecodeModule bcModule;
    bcModule.magic = kBytecodeModuleMagic;
    bcModule.version = kBytecodeVersion;
    bcModule.flags = 0;

    const uint16_t startSafeIdx =
        static_cast<uint16_t>(bcModule.addNativeFunc("Zanna.Threads.Thread.StartSafe", 2, true));
    const uint16_t safeJoinIdx =
        static_cast<uint16_t>(bcModule.addNativeFunc("Zanna.Threads.Thread.SafeJoin", 1, false));
    const uint16_t hasErrorIdx =
        static_cast<uint16_t>(bcModule.addNativeFunc("Zanna.Threads.Thread.get_HasError", 1, true));

    BytecodeFunction worker;
    worker.name = "worker_trap";
    worker.numParams = 0;
    worker.numLocals = 0;
    worker.maxStack = 1;
    worker.code.push_back(encodeOp8(BCOpcode::TRAP, static_cast<uint8_t>(TrapKind::RuntimeError)));
    worker.code.push_back(encodeOp(BCOpcode::RETURN_VOID));

    BytecodeFunction mainFunc;
    mainFunc.name = "main";
    mainFunc.numParams = 0;
    mainFunc.numLocals = 1;
    mainFunc.maxStack = 2;
    mainFunc.hasReturn = true;

    constexpr uint64_t kFuncPtrTag = 0x8000000000000000ULL;
    bcModule.i64Pool.push_back(static_cast<int64_t>(kFuncPtrTag | 0ULL));
    const uint16_t workerPtrIdx = static_cast<uint16_t>(bcModule.i64Pool.size() - 1);

    mainFunc.code.push_back(encodeOp16(BCOpcode::LOAD_I64, workerPtrIdx));
    mainFunc.code.push_back(encodeOp(BCOpcode::LOAD_NULL));
    mainFunc.code.push_back(encodeOp8_16(BCOpcode::CALL_NATIVE, 2, startSafeIdx));
    mainFunc.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 0));
    mainFunc.code.push_back(encodeOp8(BCOpcode::LOAD_LOCAL, 0));
    mainFunc.code.push_back(encodeOp8_16(BCOpcode::CALL_NATIVE, 1, safeJoinIdx));
    mainFunc.code.push_back(encodeOp8(BCOpcode::LOAD_LOCAL, 0));
    mainFunc.code.push_back(encodeOp8_16(BCOpcode::CALL_NATIVE, 1, hasErrorIdx));
    mainFunc.code.push_back(encodeOp(BCOpcode::RETURN));

    bcModule.functions.push_back(std::move(worker));
    bcModule.functionIndex["worker_trap"] = 0;
    bcModule.functions.push_back(std::move(mainFunc));
    bcModule.functionIndex["main"] = 1;

    for (bool threaded : {false, true}) {
        BytecodeVM vm;
        vm.setRuntimeBridgeEnabled(true);
        vm.setThreadedDispatch(threaded);
        vm.load(&bcModule);

        BCSlot result = vm.exec("main", {});
        assert(vm.state() == VMState::Halted);
        assert(result.i64 == 1);
    }

    std::cout << "PASSED\n";
}

/// Test that bytecode Async.RunOwned retains its argument until the worker exits.
static void test_async_run_owned_retains_bytecode_argument() {
    // Async.Run borrows its argument (native parity, VDOC-127); the retain
    // contract this test verifies belongs to Async.RunOwned.
    std::cout << "  test_async_run_owned_retains_bytecode_argument: ";

    BytecodeModule bcModule;
    bcModule.magic = kBytecodeModuleMagic;
    bcModule.version = kBytecodeVersion;
    bcModule.flags = 0;

    const uint16_t asyncRunIdx =
        static_cast<uint16_t>(bcModule.addNativeFunc("Zanna.Threads.Async.RunOwned", 2, true));
    const uint16_t futureGetIdx =
        static_cast<uint16_t>(bcModule.addNativeFunc("Zanna.Threads.Future.Get", 1, true));
    const uint16_t makeMarkerIdx =
        static_cast<uint16_t>(bcModule.addNativeFunc("test.marker.make", 0, true));
    const uint16_t releaseMarkerIdx =
        static_cast<uint16_t>(bcModule.addNativeFunc("test.marker.release", 1, false));
    const uint16_t countMarkerIdx =
        static_cast<uint16_t>(bcModule.addNativeFunc("test.marker.count", 0, true));
    const uint16_t waitGateIdx =
        static_cast<uint16_t>(bcModule.addNativeFunc("test.gate.wait", 0, false));
    const uint16_t openGateIdx =
        static_cast<uint16_t>(bcModule.addNativeFunc("test.gate.open", 0, false));

    BytecodeFunction worker;
    worker.name = "worker_async_hold";
    worker.hasReturn = true;
    worker.numParams = 1;
    worker.numLocals = 1;
    worker.maxStack = 1;
    worker.code.push_back(encodeOp8_16(BCOpcode::CALL_NATIVE, 0, waitGateIdx));
    worker.code.push_back(encodeOp(BCOpcode::LOAD_NULL));
    worker.code.push_back(encodeOp(BCOpcode::RETURN));

    BytecodeFunction mainFunc;
    mainFunc.name = "main";
    mainFunc.hasReturn = true;
    mainFunc.numParams = 0;
    mainFunc.numLocals = 5;
    mainFunc.maxStack = 2;

    constexpr uint64_t kFuncPtrTag = 0x8000000000000000ULL;
    bcModule.i64Pool.push_back(static_cast<int64_t>(kFuncPtrTag | 0ULL));
    const uint16_t workerPtrIdx = static_cast<uint16_t>(bcModule.i64Pool.size() - 1);

    mainFunc.code.push_back(encodeOp8_16(BCOpcode::CALL_NATIVE, 0, makeMarkerIdx));
    mainFunc.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 0));
    mainFunc.code.push_back(encodeOp16(BCOpcode::LOAD_I64, workerPtrIdx));
    mainFunc.code.push_back(encodeOp8(BCOpcode::LOAD_LOCAL, 0));
    mainFunc.code.push_back(encodeOp8_16(BCOpcode::CALL_NATIVE, 2, asyncRunIdx));
    mainFunc.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 1));
    mainFunc.code.push_back(encodeOp8(BCOpcode::LOAD_LOCAL, 0));
    mainFunc.code.push_back(encodeOp8_16(BCOpcode::CALL_NATIVE, 1, releaseMarkerIdx));
    mainFunc.code.push_back(encodeOp8_16(BCOpcode::CALL_NATIVE, 0, countMarkerIdx));
    mainFunc.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 2));
    mainFunc.code.push_back(encodeOp8_16(BCOpcode::CALL_NATIVE, 0, openGateIdx));
    mainFunc.code.push_back(encodeOp8(BCOpcode::LOAD_LOCAL, 1));
    mainFunc.code.push_back(encodeOp8_16(BCOpcode::CALL_NATIVE, 1, futureGetIdx));
    mainFunc.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 3));
    mainFunc.code.push_back(encodeOp8_16(BCOpcode::CALL_NATIVE, 0, countMarkerIdx));
    mainFunc.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 4));
    mainFunc.code.push_back(encodeOp8(BCOpcode::LOAD_LOCAL, 2));
    mainFunc.code.push_back(encodeOp8(BCOpcode::LOAD_I8, 10));
    mainFunc.code.push_back(encodeOp(BCOpcode::MUL_I64));
    mainFunc.code.push_back(encodeOp8(BCOpcode::LOAD_LOCAL, 4));
    mainFunc.code.push_back(encodeOp(BCOpcode::ADD_I64));
    mainFunc.code.push_back(encodeOp(BCOpcode::RETURN));

    bcModule.functions.push_back(std::move(worker));
    bcModule.functionIndex["worker_async_hold"] = 0;
    bcModule.functions.push_back(std::move(mainFunc));
    bcModule.functionIndex["main"] = 1;

    auto registerExtern = [](const char *name,
                             std::initializer_list<SigKind> params,
                             std::initializer_list<SigKind> rets,
                             void *fn) {
        il::vm::ExternDesc ext;
        ext.name = name;
        ext.signature = make_signature(name, params, rets);
        ext.fn = fn;
        (void)il::vm::registerExternIn(il::vm::processGlobalExternRegistry(), ext);
    };
    registerExtern(
        "test.marker.make", {}, {SigKind::Ptr}, reinterpret_cast<void *>(&make_async_arg_marker));
    registerExtern("test.marker.release",
                   {SigKind::Ptr},
                   {},
                   reinterpret_cast<void *>(&release_async_arg_marker));
    registerExtern(
        "test.marker.count", {}, {SigKind::I64}, reinterpret_cast<void *>(&count_async_arg_marker));
    registerExtern("test.gate.wait", {}, {}, reinterpret_cast<void *>(&wait_async_arg_native));
    registerExtern("test.gate.open", {}, {}, reinterpret_cast<void *>(&open_async_arg_native));

    for (bool threaded : {false, true}) {
        reset_async_arg_state();

        BytecodeVM vm;
        vm.setRuntimeBridgeEnabled(true);
        vm.setThreadedDispatch(threaded);
        vm.load(&bcModule);

        BCSlot result = vm.exec("main", {});
        if (vm.state() != VMState::Halted) {
            std::cerr << "async arg retention test trapped (threaded=" << threaded
                      << "): " << vm.trapMessage() << "\n";
        }
        assert(vm.state() == VMState::Halted);
        assert(result.i64 == 1);
    }

    std::cout << "PASSED\n";
}

/// Test that bytecode Thread.StartOwned / StartSafeOwned retain managed args.
static void test_thread_start_owned_retains_bytecode_argument() {
    std::cout << "  test_thread_start_owned_retains_bytecode_argument: ";

    auto runCase = [](const char *startName, const char *joinName) {
        BytecodeModule bcModule;
        bcModule.magic = kBytecodeModuleMagic;
        bcModule.version = kBytecodeVersion;
        bcModule.flags = 0;

        const uint16_t startIdx = static_cast<uint16_t>(bcModule.addNativeFunc(startName, 2, true));
        const uint16_t joinIdx = static_cast<uint16_t>(bcModule.addNativeFunc(joinName, 1, false));
        const uint16_t makeMarkerIdx =
            static_cast<uint16_t>(bcModule.addNativeFunc("test.marker.make", 0, true));
        const uint16_t releaseMarkerIdx =
            static_cast<uint16_t>(bcModule.addNativeFunc("test.marker.release", 1, false));
        const uint16_t countMarkerIdx =
            static_cast<uint16_t>(bcModule.addNativeFunc("test.marker.count", 0, true));
        const uint16_t waitGateIdx =
            static_cast<uint16_t>(bcModule.addNativeFunc("test.gate.wait", 0, false));
        const uint16_t openGateIdx =
            static_cast<uint16_t>(bcModule.addNativeFunc("test.gate.open", 0, false));

        BytecodeFunction worker;
        worker.name = "worker_owned_hold";
        worker.numParams = 1;
        worker.numLocals = 1;
        worker.maxStack = 1;
        worker.code.push_back(encodeOp8_16(BCOpcode::CALL_NATIVE, 0, waitGateIdx));
        worker.code.push_back(encodeOp(BCOpcode::RETURN_VOID));

        BytecodeFunction mainFunc;
        mainFunc.name = "main";
        mainFunc.hasReturn = true;
        mainFunc.numParams = 0;
        mainFunc.numLocals = 4;
        mainFunc.maxStack = 2;

        constexpr uint64_t kFuncPtrTag = 0x8000000000000000ULL;
        bcModule.i64Pool.push_back(static_cast<int64_t>(kFuncPtrTag | 0ULL));
        const uint16_t workerPtrIdx = static_cast<uint16_t>(bcModule.i64Pool.size() - 1);

        mainFunc.code.push_back(encodeOp8_16(BCOpcode::CALL_NATIVE, 0, makeMarkerIdx));
        mainFunc.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 0));
        mainFunc.code.push_back(encodeOp16(BCOpcode::LOAD_I64, workerPtrIdx));
        mainFunc.code.push_back(encodeOp8(BCOpcode::LOAD_LOCAL, 0));
        mainFunc.code.push_back(encodeOp8_16(BCOpcode::CALL_NATIVE, 2, startIdx));
        mainFunc.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 1));
        mainFunc.code.push_back(encodeOp8(BCOpcode::LOAD_LOCAL, 0));
        mainFunc.code.push_back(encodeOp8_16(BCOpcode::CALL_NATIVE, 1, releaseMarkerIdx));
        mainFunc.code.push_back(encodeOp8_16(BCOpcode::CALL_NATIVE, 0, countMarkerIdx));
        mainFunc.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 2));
        mainFunc.code.push_back(encodeOp8_16(BCOpcode::CALL_NATIVE, 0, openGateIdx));
        mainFunc.code.push_back(encodeOp8(BCOpcode::LOAD_LOCAL, 1));
        mainFunc.code.push_back(encodeOp8_16(BCOpcode::CALL_NATIVE, 1, joinIdx));
        mainFunc.code.push_back(encodeOp8_16(BCOpcode::CALL_NATIVE, 0, countMarkerIdx));
        mainFunc.code.push_back(encodeOp8(BCOpcode::STORE_LOCAL, 3));
        mainFunc.code.push_back(encodeOp8(BCOpcode::LOAD_LOCAL, 2));
        mainFunc.code.push_back(encodeOp8(BCOpcode::LOAD_I8, 10));
        mainFunc.code.push_back(encodeOp(BCOpcode::MUL_I64));
        mainFunc.code.push_back(encodeOp8(BCOpcode::LOAD_LOCAL, 3));
        mainFunc.code.push_back(encodeOp(BCOpcode::ADD_I64));
        mainFunc.code.push_back(encodeOp(BCOpcode::RETURN));

        bcModule.functions.push_back(std::move(worker));
        bcModule.functionIndex["worker_owned_hold"] = 0;
        bcModule.functions.push_back(std::move(mainFunc));
        bcModule.functionIndex["main"] = 1;

        auto registerExtern = [](const char *name,
                                 std::initializer_list<SigKind> params,
                                 std::initializer_list<SigKind> rets,
                                 void *fn) {
            il::vm::ExternDesc ext;
            ext.name = name;
            ext.signature = make_signature(name, params, rets);
            ext.fn = fn;
            (void)il::vm::registerExternIn(il::vm::processGlobalExternRegistry(), ext);
        };
        registerExtern("test.marker.make",
                       {},
                       {SigKind::Ptr},
                       reinterpret_cast<void *>(&make_async_arg_marker));
        registerExtern("test.marker.release",
                       {SigKind::Ptr},
                       {},
                       reinterpret_cast<void *>(&release_async_arg_marker));
        registerExtern("test.marker.count",
                       {},
                       {SigKind::I64},
                       reinterpret_cast<void *>(&count_async_arg_marker));
        registerExtern("test.gate.wait", {}, {}, reinterpret_cast<void *>(&wait_async_arg_native));
        registerExtern("test.gate.open", {}, {}, reinterpret_cast<void *>(&open_async_arg_native));

        for (bool threaded : {false, true}) {
            reset_async_arg_state();

            BytecodeVM vm;
            vm.setRuntimeBridgeEnabled(true);
            vm.setThreadedDispatch(threaded);
            vm.load(&bcModule);

            BCSlot result = vm.exec("main", {});
            if (vm.state() != VMState::Halted) {
                std::cerr << "owned thread retention test trapped for " << startName
                          << " (threaded=" << threaded << "): " << vm.trapMessage() << "\n";
            }
            assert(vm.state() == VMState::Halted);
            assert(result.i64 == 1);
        }
    };

    runCase("Zanna.Threads.Thread.StartOwned", "Zanna.Threads.Thread.Join");
    runCase("Zanna.Threads.Thread.StartSafeOwned", "Zanna.Threads.Thread.SafeJoin");

    std::cout << "PASSED\n";
}

static void test_branch_arguments_are_atomic() {
    std::cout << "  test_branch_arguments_are_atomic: ";

    BytecodeModule bcModule = compileAssumingVerified(createBranchArgumentSwapModule());

    for (bool threaded : {false, true}) {
        for (bool trusted : {false, true}) {
            BytecodeVM vm;
            vm.setThreadedDispatch(threaded);
            vm.setTrustedDispatch(trusted);
            vm.load(&bcModule);
            BCSlot result = vm.exec("branch_arg_swap", {BCSlot::fromInt(3), BCSlot::fromInt(5)});
            assert(vm.state() == VMState::Halted);
            assert(result.i64 == 3);
        }
    }

    std::cout << "PASSED\n";
}

/// @brief Main.
int main() {
    ZANNA_DISABLE_ABORT_DIALOG();
    std::cout << "Running bytecode VM tests...\n";

    test_bytecode_encoding();
    test_add_function();
    test_unified_handlers_reinstall_only_after_registry_change();
    test_abs_function();
    test_fib_small();
    test_fib_benchmark();
    test_dispatch_benchmark();
    test_native_calls();
    test_runtime_bridge_string_aliasing();
    test_string_memory_lifetime();
    test_string_release_call_lifetime();
    test_string_stack_op_lifetime();
    test_global_address_storage();
    test_vm_numeric_edge_regressions();
    test_vm_safety_trap_regressions();
    test_native_multi_args();
    test_native_wide_index();
    test_exception_handling();
    test_unhandled_trap();
    test_trap_from_err_maps_legacy_error_code();
    test_eh_pop();
    test_runtime_bridge_trap_dispatch();
    test_resume_next_and_trap_metadata();
    test_resume_label_consumes_token();
    test_invalid_pool_and_global_indices();
    test_debug_api();
    test_entry_arity_mismatch();
    test_invalid_branch_target();
    test_truncated_extended_instruction();
    test_branch_target_rejects_inline_word();
    test_alloca_typed_memory_range_check();
    test_indirect_call_arity_mismatch_traps();
    test_trusted_dispatch_toggle();
    test_native_refs_cache_runtime_metadata();
    test_array_fast_path_bytecode_ops();
    test_branch_arguments_are_atomic();
    test_thread_start_safe_reports_bytecode_trap();
    test_async_run_owned_retains_bytecode_argument();
    test_thread_start_owned_retains_bytecode_argument();

    std::cout << "All bytecode VM tests PASSED!\n";
    return 0;
}
