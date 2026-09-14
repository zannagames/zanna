//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/NativeAllocaZeroInit.cpp
// Purpose: Insert the stores that zero each alloca's memory before native
//          backends map allocas to uninitialised frame slots.
// Key invariants:
//   - Stores follow the alloca directly, in its block, and write exactly the
//     allocation's bytes with in-bounds accesses of matching width.
//   - A counted loop clears large allocations; it splits the block, and the
//     original terminator moves to the continuation block.
// Ownership/Lifetime:
//   - Mutates the caller's module in place.
// Links: src/codegen/common/NativeAllocaZeroInit.hpp, docs/il/il-guide.md
//
//===----------------------------------------------------------------------===//

#include "codegen/common/NativeAllocaZeroInit.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/Type.hpp"
#include "il/core/Value.hpp"
#include "il/utils/Utils.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/// @file
/// @brief Implements native alloca zero-initialisation.

namespace zanna::codegen::common {
namespace {

using ::il::core::BasicBlock;
using ::il::core::Function;
using ::il::core::Instr;
using ::il::core::Module;
using ::il::core::Opcode;
using ::il::core::Param;
using ::il::core::Type;
using ::il::core::Value;

/// Allocations up to this size are cleared by a straight run of stores.
constexpr int64_t kMaxUnrolledBytes = 512;

/// @brief Reserve a function value id with a debug name.
/// @details Callers must first extend the value-name table past every id in use
///          (see zeroInitFunction); optimizer passes create ids beyond the table.
/// @param fn Function whose value-name table grows.
/// @param name Synthetic value name.
/// @return The new value id.
unsigned reserveTemp(Function &fn, const std::string &name) {
    const auto id = static_cast<unsigned>(fn.valueNames.size());
    fn.valueNames.push_back(name);
    return id;
}

/// @brief Build a store of zero with @p kind width to @p ptr.
/// @param ptr Destination address.
/// @param kind Stored scalar type.
/// @param loc Source location to attach.
/// @return The store instruction.
Instr makeZeroStore(Value ptr, Type::Kind kind, const ::il::support::SourceLoc &loc) {
    Instr store;
    store.op = Opcode::Store;
    store.type = Type(kind);
    store.operands.push_back(std::move(ptr));
    store.operands.push_back(kind == Type::Kind::I1 ? Value::constBool(false) : Value::constInt(0));
    store.loc = loc;
    return store;
}

/// @brief Build `%result = gep base, offset`.
/// @param result Result value id.
/// @param base Base address.
/// @param offset Byte offset value.
/// @param loc Source location to attach.
/// @return The GEP instruction.
Instr makeGep(unsigned result, Value base, Value offset, const ::il::support::SourceLoc &loc) {
    Instr gep;
    gep.result = result;
    gep.op = Opcode::GEP;
    gep.type = Type(Type::Kind::Ptr);
    gep.operands.push_back(std::move(base));
    gep.operands.push_back(std::move(offset));
    gep.loc = loc;
    return gep;
}

/// @brief Append stores clearing bytes [@p from, @p size) of @p ptr.
/// @details Whole 8-byte chunks use `i64` stores; the tail uses `i32`, `i16`, and
///          `i1` stores so no access passes the end of the allocation.
/// @param fn Function receiving temporaries.
/// @param out Instruction list receiving the stores.
/// @param ptr Allocation address.
/// @param from First byte to clear.
/// @param size Allocation size in bytes.
/// @param loc Source location to attach.
void appendZeroStores(Function &fn,
                      std::vector<Instr> &out,
                      const Value &ptr,
                      int64_t from,
                      int64_t size,
                      const ::il::support::SourceLoc &loc) {
    /// Address of byte @p offset of the allocation.
    auto addressAt = [&](int64_t offset) {
        if (offset == 0)
            return ptr;
        const unsigned id =
            reserveTemp(fn, "__zero." + std::to_string(ptr.id) + "." + std::to_string(offset));
        out.push_back(makeGep(id, ptr, Value::constInt(offset), loc));
        return Value::temp(id);
    };

    int64_t offset = from;
    for (; offset + 8 <= size; offset += 8)
        out.push_back(makeZeroStore(addressAt(offset), Type::Kind::I64, loc));
    if (offset + 4 <= size) {
        out.push_back(makeZeroStore(addressAt(offset), Type::Kind::I32, loc));
        offset += 4;
    }
    if (offset + 2 <= size) {
        out.push_back(makeZeroStore(addressAt(offset), Type::Kind::I16, loc));
        offset += 2;
    }
    if (offset < size)
        out.push_back(makeZeroStore(addressAt(offset), Type::Kind::I1, loc));
}

/// @brief Whether @p instr is an alloca of constant positive size with a result.
/// @param instr Instruction to test.
/// @param size Receives the size in bytes.
/// @return True for a zeroable alloca.
bool zeroableAlloca(const Instr &instr, int64_t &size) {
    if (instr.op != Opcode::Alloca || !instr.result || instr.operands.empty())
        return false;
    const Value &count = instr.operands[0];
    if (count.kind != Value::Kind::ConstInt || count.i64 <= 0)
        return false;
    size = count.i64;
    return true;
}

/// @brief Produce a function-unique block label from @p stem.
/// @param fn Function whose labels must stay unique.
/// @param stem Label stem.
/// @param counter Per-function counter advanced on each call.
/// @return Label not used by any block of @p fn.
std::string uniqueLabel(const Function &fn, const std::string &stem, unsigned &counter) {
    for (;;) {
        std::string label = stem + "." + std::to_string(counter++);
        bool taken = false;
        for (const auto &bb : fn.blocks) {
            if (bb.label == label) {
                taken = true;
                break;
            }
        }
        if (!taken)
            return label;
    }
}

/// @brief Zero the allocas of one function.
/// @param fn Function to rewrite.
/// @return True when any alloca was given zeroing stores.
bool zeroInitFunction(Function &fn) {
    // IL passes allocate ids past the value-name table without naming them, so
    // cover every id in use before reserveTemp hands out table-size ids.
    const unsigned firstFreeId = zanna::il::nextTempId(fn);
    if (fn.valueNames.size() < firstFreeId)
        fn.valueNames.resize(firstFreeId);

    bool changed = false;
    unsigned labelCounter = 0;
    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        std::vector<Instr> original;
        original.reserve(fn.blocks[bi].instructions.size());
        for (auto &instr : fn.blocks[bi].instructions)
            original.push_back(std::move(instr));

        std::vector<Instr> rewritten;
        rewritten.reserve(original.size());
        bool split = false;
        for (size_t ii = 0; ii < original.size(); ++ii) {
            int64_t size = 0;
            const bool zeroable = zeroableAlloca(original[ii], size);
            const ::il::support::SourceLoc loc = original[ii].loc;
            rewritten.push_back(std::move(original[ii]));
            if (!zeroable)
                continue;
            changed = true;
            const Value ptr = Value::temp(*rewritten.back().result);
            if (size <= kMaxUnrolledBytes) {
                appendZeroStores(fn, rewritten, ptr, 0, size, loc);
                continue;
            }

            // Large allocation: clear the 8-byte chunks with a counted loop, then the
            // tail, and continue the block's remaining instructions after the loop.
            const int64_t chunkBytes = (size / 8) * 8;
            const std::string suffix = std::to_string(ptr.id);
            const std::string headLabel = uniqueLabel(fn, "__zero.head." + suffix, labelCounter);
            const std::string bodyLabel = uniqueLabel(fn, "__zero.body." + suffix, labelCounter);
            const std::string doneLabel = uniqueLabel(fn, "__zero.done." + suffix, labelCounter);

            Instr enter;
            enter.op = Opcode::Br;
            enter.type = Type(Type::Kind::Void);
            enter.addBranchTarget(headLabel, {Value::constInt(0)});
            enter.loc = loc;
            rewritten.push_back(std::move(enter));

            BasicBlock head;
            head.label = headLabel;
            Param index;
            index.name = "__zero.i." + suffix;
            index.type = Type(Type::Kind::I64);
            index.id = reserveTemp(fn, index.name);
            head.params.push_back(index);
            const unsigned more = reserveTemp(fn, "__zero.more." + suffix);
            Instr compare;
            compare.result = more;
            compare.op = Opcode::SCmpLT;
            compare.type = Type(Type::Kind::I1);
            compare.operands = {Value::temp(index.id), Value::constInt(chunkBytes)};
            compare.loc = loc;
            head.instructions.push_back(std::move(compare));
            Instr test;
            test.op = Opcode::CBr;
            test.type = Type(Type::Kind::Void);
            test.operands.push_back(Value::temp(more));
            test.addBranchTarget(bodyLabel);
            test.addBranchTarget(doneLabel);
            test.loc = loc;
            head.instructions.push_back(std::move(test));
            head.terminated = true;

            BasicBlock body;
            body.label = bodyLabel;
            const unsigned slot = reserveTemp(fn, "__zero.slot." + suffix);
            body.instructions.push_back(makeGep(slot, ptr, Value::temp(index.id), loc));
            body.instructions.push_back(makeZeroStore(Value::temp(slot), Type::Kind::I64, loc));
            const unsigned next = reserveTemp(fn, "__zero.next." + suffix);
            Instr advance;
            advance.result = next;
            advance.op = Opcode::IAddOvf;
            advance.type = Type(Type::Kind::I64);
            advance.operands = {Value::temp(index.id), Value::constInt(8)};
            advance.loc = loc;
            body.instructions.push_back(std::move(advance));
            Instr loop;
            loop.op = Opcode::Br;
            loop.type = Type(Type::Kind::Void);
            loop.addBranchTarget(headLabel, {Value::temp(next)});
            loop.loc = loc;
            body.instructions.push_back(std::move(loop));
            body.terminated = true;

            std::vector<Instr> doneInstrs;
            appendZeroStores(fn, doneInstrs, ptr, chunkBytes, size, loc);
            BasicBlock done;
            done.label = doneLabel;
            for (auto &instr : doneInstrs)
                done.instructions.push_back(std::move(instr));
            for (size_t rest = ii + 1; rest < original.size(); ++rest)
                done.instructions.push_back(std::move(original[rest]));
            done.terminated = fn.blocks[bi].terminated;

            BasicBlock &current = fn.blocks[bi];
            current.instructions.clear();
            for (auto &instr : rewritten)
                current.instructions.push_back(std::move(instr));
            current.terminated = true;

            fn.blocks.insert(fn.blocks.begin() + static_cast<std::ptrdiff_t>(bi + 1),
                             std::move(head));
            fn.blocks.insert(fn.blocks.begin() + static_cast<std::ptrdiff_t>(bi + 2),
                             std::move(body));
            fn.blocks.insert(fn.blocks.begin() + static_cast<std::ptrdiff_t>(bi + 3),
                             std::move(done));
            // Continue with the continuation block, which may hold more allocas.
            bi += 2;
            split = true;
            break;
        }

        if (!split) {
            BasicBlock &current = fn.blocks[bi];
            current.instructions.clear();
            for (auto &instr : rewritten)
                current.instructions.push_back(std::move(instr));
        }
    }
    return changed;
}

} // namespace

/// @copydoc zeroInitAllocas
bool zeroInitAllocas(Module &module) {
    bool changed = false;
    for (auto &fn : module.functions)
        changed = zeroInitFunction(fn) || changed;
    return changed;
}

} // namespace zanna::codegen::common
