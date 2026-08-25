//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/codegen/x86_64/test_null_trap.cpp
// Purpose: Verify that loads/stores through a non-frame base emit the
//          null-page guard (cmpq $4096; jb .Ltrap_null_*) with a shared
//          trap block calling rt_trap_null, that a base is guarded at most
//          once per block, and that alloca-based accesses stay unguarded.
// Key invariants:
//   - The IL spec requires null loads/stores to trap deterministically;
//     native code must match the VMs' null/low-page rule.
// Ownership/Lifetime:
//   - The test builds IL modules locally and inspects the assembly text.
// Links: src/codegen/x86_64/Lowering.EmitCommon.cpp (emitNullAddressGuard)
//
//===----------------------------------------------------------------------===//

#include "codegen/x86_64/Backend.hpp"

#include <iostream>
#include <string>

namespace zanna::codegen::x64 {
namespace {

[[nodiscard]] ILValue makeParam(int id, ILValue::Kind kind) noexcept {
    ILValue v{};
    v.kind = kind;
    v.id = id;
    return v;
}

[[nodiscard]] ILValue makeImmI64(long long val) noexcept {
    ILValue v{};
    v.kind = ILValue::Kind::I64;
    v.id = -1;
    v.i64 = val;
    return v;
}

[[nodiscard]] ILValue makeValueRef(int id, ILValue::Kind kind) noexcept {
    ILValue v{};
    v.kind = kind;
    v.id = id;
    return v;
}

// v = load [p]; store [p] <- v; ret v — one pointer-param base used twice, so
// exactly one guard must be emitted for the block.
[[nodiscard]] ILFunction makePointerAccessFunction() {
    ILValue p = makeParam(0, ILValue::Kind::PTR);

    ILInstr ld{};
    ld.opcode = "load";
    ld.resultId = 1;
    ld.resultKind = ILValue::Kind::I64;
    ld.ops = {p};

    ILInstr st{};
    st.opcode = "store";
    st.ops = {p, makeValueRef(1, ILValue::Kind::I64)};

    ILInstr ret{};
    ret.opcode = "ret";
    ret.ops = {makeValueRef(1, ILValue::Kind::I64)};

    ILBlock entry{};
    entry.name = "entry";
    entry.paramIds = {p.id};
    entry.paramKinds = {p.kind};
    entry.instrs = {ld, st, ret};

    ILFunction fn{};
    fn.name = "null_guard";
    fn.blocks = {entry};
    return fn;
}

// %s = alloca 8; store [%s] <- 7; %v = load [%s]; ret %v — a provably mapped
// stack base must not be guarded.
[[nodiscard]] ILFunction makeFrameOnlyFunction() {
    ILInstr al{};
    al.opcode = "alloca";
    al.resultId = 0;
    al.resultKind = ILValue::Kind::PTR;
    al.ops = {makeImmI64(8)};

    ILInstr st{};
    st.opcode = "store";
    st.ops = {makeValueRef(0, ILValue::Kind::PTR), makeImmI64(7)};

    ILInstr ld{};
    ld.opcode = "load";
    ld.resultId = 1;
    ld.resultKind = ILValue::Kind::I64;
    ld.ops = {makeValueRef(0, ILValue::Kind::PTR)};

    ILInstr ret{};
    ret.opcode = "ret";
    ret.ops = {makeValueRef(1, ILValue::Kind::I64)};

    ILBlock entry{};
    entry.name = "entry";
    entry.instrs = {al, st, ld, ret};

    ILFunction fn{};
    fn.name = "frame_only";
    fn.blocks = {entry};
    return fn;
}

[[nodiscard]] std::size_t countOccurrences(const std::string &text, const std::string &needle) {
    std::size_t count = 0;
    for (std::size_t pos = text.find(needle); pos != std::string::npos;
         pos = text.find(needle, pos + needle.size()))
        ++count;
    return count;
}

} // namespace
} // namespace zanna::codegen::x64

int main() {
    using namespace zanna::codegen::x64;

    // --- Guarded pointer access -------------------------------------------
    {
        ILModule module{};
        module.funcs = {makePointerAccessFunction()};
        const CodegenResult result = emitModuleToAssembly(module, {});
        if (!result.errors.empty()) {
            std::cerr << result.errors;
            return 1;
        }
        const std::string &text = result.asmText;
        if (countOccurrences(text, "cmpq $4096") != 1) {
            std::cerr << "Expected exactly one null-page compare for a base "
                         "reused within one block:\n"
                      << text;
            return 2;
        }
        if (text.find(".Ltrap_null_null_guard") == std::string::npos ||
            text.find("jb ") == std::string::npos) {
            std::cerr << "Expected an unsigned-below branch to the null trap block:\n" << text;
            return 3;
        }
        if (text.find("rt_trap_null") == std::string::npos) {
            std::cerr << "Expected the trap block to call rt_trap_null:\n" << text;
            return 4;
        }
    }

    // --- Alloca-based access stays unguarded ------------------------------
    {
        ILModule module{};
        module.funcs = {makeFrameOnlyFunction()};
        const CodegenResult result = emitModuleToAssembly(module, {});
        if (!result.errors.empty()) {
            std::cerr << result.errors;
            return 5;
        }
        const std::string &text = result.asmText;
        if (text.find(".Ltrap_null") != std::string::npos ||
            text.find("cmpq $4096") != std::string::npos) {
            std::cerr << "Alloca-based accesses must not emit null-page guards:\n" << text;
            return 6;
        }
    }

    return 0;
}
