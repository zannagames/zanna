//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lower/common/CommonLowering.cpp
// Purpose: Implement reusable IL emission helpers shared across BASIC lowering.
// Key invariants: All helpers assume the Lowerer selected an active basic block
//                 before emitting instructions and respect the current source
//                 location tracked by the lowering context.
// Ownership/Lifetime: Borrows the Lowerer without taking ownership of IR
//                     builders, functions, or AST nodes.
// Links: docs/internals/codemap.md, docs/internals/architecture.md#cpp-overview
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements reusable primitive IL emission for BASIC lowerers.

#include "frontends/basic/lower/common/CommonLowering.hpp"

#include "frontends/basic/Lowerer.hpp"

#include "zanna/il/Module.hpp"

#include <cassert>
#include <utility>

using namespace il::core;

namespace il::frontends::basic::lower::common {

/// @brief Construct a helper that shares lowering utilities across emitters.
/// @details Stores a pointer to the owning @ref Lowerer so each helper method
///          can access contextual data such as the current basic block, source
///          location, and temporary ID allocator.
/// @param lowerer Owning lowering pipeline that provides context.
CommonLowering::CommonLowering(Lowerer &lowerer) noexcept : lowerer_(&lowerer) {}

/// @brief Obtain the canonical boolean IL type used by the BASIC frontend.
/// @details The BASIC dialect models booleans as 1-bit integers; this helper
///          centralises the type construction so all emission sites agree on the
///          representation.
/// @return IL type descriptor for a 1-bit integer.
CommonLowering::Type CommonLowering::ilBoolTy() const {
    return Type(Type::Kind::I1);
}

/// @brief Emit a boolean constant value.
/// @details Generates a `Trunc1` instruction so that constants flow through the
///          same pipeline as computed booleans, ensuring downstream passes see a
///          consistent opcode pattern.
/// @param v Boolean literal to encode.
/// @return Value referencing the emitted constant.
CommonLowering::Value CommonLowering::emitBoolConst(bool v) {
    return emitUnary(Opcode::Trunc1, ilBoolTy(), Value::constInt(v ? 1 : 0));
}

/// @brief Materialise a boolean by branching and capturing control-flow
///        outcomes.
/// @details Allocates a temporary slot, emits paired basic blocks for the then
///          and else paths, and invokes the provided callbacks to populate each
///          branch.  Each branch stores into the slot before jumping to the join
///          block where the stored value is reloaded.
/// @param emitThen Callback that emits the `true` branch and stores a value.
/// @param emitElse Callback that emits the `false` branch and stores a value.
/// @param thenLabelBase Base label used to name the `true` block.
/// @param elseLabelBase Base label used to name the `false` block.
/// @param joinLabelBase Base label used for the join continuation block.
/// @return Value holding the boolean loaded from the join block.
CommonLowering::Value CommonLowering::emitBoolFromBranches(
    const std::function<void(Value)> &emitThen,
    const std::function<void(Value)> &emitElse,
    std::string_view thenLabelBase,
    std::string_view elseLabelBase,
    std::string_view joinLabelBase) {
    auto &ctx = lowerer_->context();
    Value slot = emitAlloca(1);

    std::string thenLbl = makeBlockLabel(thenLabelBase);
    std::string elseLbl = makeBlockLabel(elseLabelBase);
    std::string joinLbl = makeBlockLabel(joinLabelBase);

    Function *func = ctx.function();
    assert(func && "emitBoolFromBranches requires an active function");

    BasicBlock *thenBlk = &lowerer_->builder->addBlock(*func, thenLbl);
    BasicBlock *elseBlk = &lowerer_->builder->addBlock(*func, elseLbl);
    BasicBlock *joinBlk = &lowerer_->builder->addBlock(*func, joinLbl);

    ctx.setCurrent(thenBlk);
    emitThen(slot);
    if (ctx.current() && !ctx.current()->terminated)
        emitBr(joinBlk);

    ctx.setCurrent(elseBlk);
    emitElse(slot);
    if (ctx.current() && !ctx.current()->terminated)
        emitBr(joinBlk);

    ctx.setCurrent(joinBlk);
    return emitLoad(ilBoolTy(), slot);
}

/// @brief Reserve stack storage within the current function.
/// @details Synthesises an `Alloca` instruction with the requested byte count
///          and appends it to the active basic block.  The helper also
///          associates the instruction with the current source location to aid
///          diagnostics.
/// @param bytes Number of bytes to allocate.
/// @return Value referencing the allocated pointer temporary.
CommonLowering::Value CommonLowering::emitAlloca(int bytes) {
    unsigned id = lowerer_->nextTempId();
    Instr in;
    in.result = id;
    in.op = Opcode::Alloca;
    in.type = Type(Type::Kind::Ptr);
    in.operands.push_back(Value::constInt(bytes));
    in.loc = lowerer_->curLoc;
    BasicBlock *block = lowerer_->context().current();
    assert(block && "emitAlloca requires an active block");
    block->instructions.push_back(in);
    return Value::temp(id);
}

/// @brief Emit a load from memory at the given address.
/// @details Creates a `Load` instruction with the supplied result type, assigns
///          it a fresh temporary ID, and appends it to the current block while
///          preserving the active source location.
/// @param ty Type of the value being loaded.
/// @param addr Pointer operand describing the load address.
/// @return Value representing the loaded temporary.
CommonLowering::Value CommonLowering::emitLoad(Type ty, Value addr) {
    unsigned id = lowerer_->nextTempId();
    Instr in;
    in.result = id;
    in.op = Opcode::Load;
    in.type = ty;
    in.operands.push_back(addr);
    in.loc = lowerer_->curLoc;
    BasicBlock *block = lowerer_->context().current();
    assert(block && "emitLoad requires an active block");
    block->instructions.push_back(in);
    return Value::temp(id);
}

/// @brief Store a value to memory within the active basic block.
/// @details Emits a `Store` instruction that records the address and value
///          operands, tagging the instruction with the current source location
///          before appending it to the block.
/// @param ty Type of the value to store.
/// @param addr Destination pointer.
/// @param val Value being written.
void CommonLowering::emitStore(Type ty, Value addr, Value val) {
    Instr in;
    in.op = Opcode::Store;
    in.type = ty;
    in.operands = {addr, val};
    in.loc = lowerer_->curLoc;
    BasicBlock *block = lowerer_->context().current();
    assert(block && "emitStore requires an active block");
    block->instructions.push_back(in);
}

/// @brief Emit a binary IL instruction.
/// @details Appends an instruction using @p op that consumes @p lhs and @p rhs,
///          produces a new temporary of type @p ty, and records the current
///          source location.
/// @param op Opcode describing the binary operation.
/// @param ty Result type of the operation.
/// @param lhs Left-hand operand value.
/// @param rhs Right-hand operand value.
/// @return Value referencing the resulting temporary.
CommonLowering::Value CommonLowering::emitBinary(Opcode op, Type ty, Value lhs, Value rhs) {
    unsigned id = lowerer_->nextTempId();
    Instr in;
    in.result = id;
    in.op = op;
    in.type = ty;
    in.operands = {lhs, rhs};
    in.loc = lowerer_->curLoc;
    BasicBlock *block = lowerer_->context().current();
    assert(block && "emitBinary requires an active block");
    block->instructions.push_back(in);
    return Value::temp(id);
}

/// @brief Emit a unary IL instruction.
/// @details Creates an instruction with opcode @p op, result type @p ty, and
///          operand @p val, attaches the current source location, and inserts it
///          into the active block.
/// @param op Opcode describing the unary transform.
/// @param ty Result type of the operation.
/// @param val Operand value consumed by the operation.
/// @return Value referencing the resulting temporary.
CommonLowering::Value CommonLowering::emitUnary(Opcode op, Type ty, Value val) {
    unsigned id = lowerer_->nextTempId();
    Instr in;
    in.result = id;
    in.op = op;
    in.type = ty;
    in.operands = {val};
    in.loc = lowerer_->curLoc;
    BasicBlock *block = lowerer_->context().current();
    assert(block && "emitUnary requires an active block");
    block->instructions.push_back(in);
    return Value::temp(id);
}

/// @brief Produce a 64-bit integer constant value.
/// @details Wraps the helper so call sites avoid direct construction of
///          constant values, improving readability.
/// @param v Literal integer to materialise.
/// @return Value representing the constant.
CommonLowering::Value CommonLowering::emitConstI64(std::int64_t v) const {
    return Value::constInt(v);
}

/// @brief Zero-extend a boolean to a 64-bit integer.
/// @details Emits a `Zext1` instruction so boolean values can participate in
///          integer arithmetic required by BASIC semantics.
/// @param val Boolean operand to extend.
/// @return 64-bit integer value containing the zero-extended operand.
CommonLowering::Value CommonLowering::emitZext1ToI64(Value val) {
    return emitUnary(Opcode::Zext1, Type(Type::Kind::I64), val);
}

/// @brief Emit a saturating integer subtraction with overflow checks.
/// @details Uses the `ISubOvf` opcode so overflow is surfaced through the
///          runtime trap semantics instead of silently wrapping.
/// @param lhs Left-hand operand.
/// @param rhs Right-hand operand.
/// @return Value representing the subtraction result.
CommonLowering::Value CommonLowering::emitISub(Value lhs, Value rhs) {
    return emitBinary(Opcode::ISubOvf, Type(Type::Kind::I64), lhs, rhs);
}

/// @brief Convert a BASIC boolean into its integer logical representation.
/// @details BASIC treats true as `-1`; this helper emits the required
///          zero-extension and negation sequence or produces an immediate
///          constant when lowering occurs outside of a block.
/// @param b1 Boolean operand to translate.
/// @return Integer value encoding the BASIC logical semantics.
CommonLowering::Value CommonLowering::emitBasicLogicalI64(Value b1) {
    if (lowerer_->context().current() == nullptr) {
        if (b1.kind == Value::Kind::ConstInt)
            return Value::constInt(b1.i64 != 0 ? -1 : 0);
        return Value::constInt(0);
    }
    Value i64zero = emitConstI64(0);
    Value zext = emitZext1ToI64(b1);
    return emitISub(i64zero, zext);
}

/// @brief Emit a negation with overflow checking.
/// @details Expressed as subtraction from zero so the same overflow logic as
///          @ref emitISub applies, ensuring runtime traps occur when the minimum
///          representable value is negated.
/// @param ty Result type of the negation.
/// @param val Operand to negate.
/// @return Value capturing the negated result.
CommonLowering::Value CommonLowering::emitCheckedNeg(Type ty, Value val) {
    return emitBinary(Opcode::ISubOvf, ty, Value::constInt(0), val);
}

/// @brief Emit an unconditional branch to @p target.
/// @details Ensures the block has a label, appends a `Br` instruction, and
///          marks the current block as terminated to prevent additional
///          instructions from being emitted accidentally.
/// @param target Destination block for the branch.
void CommonLowering::emitBr(BasicBlock *target) {
    BasicBlock *block = lowerer_->context().current();
    assert(block && "emitBr requires an active block");

    if (block == target)
        return;

    Instr in;
    in.op = Opcode::Br;
    in.type = Type(Type::Kind::Void);
    if (target->label.empty())
        target->label = lowerer_->nextFallbackBlockLabel();
    in.addBranchTarget(target->label);
    in.loc = lowerer_->curLoc;
    block->instructions.push_back(in);
    block->terminated = true;
}

/// @brief Emit a conditional branch based on @p cond.
/// @details Produces a `CBr` instruction referencing both successor labels and
///          marks the current block as terminated.
/// @param cond Boolean condition controlling the branch.
/// @param t Destination taken when the condition evaluates to true.
/// @param f Destination taken when the condition evaluates to false.
void CommonLowering::emitCBr(Value cond, BasicBlock *t, BasicBlock *f) {
    Instr in;
    in.op = Opcode::CBr;
    in.type = Type(Type::Kind::Void);
    in.operands.push_back(cond);
    // Ensure both successors have concrete labels, mirroring emitBr().
    if (t->label.empty())
        t->label = lowerer_->nextFallbackBlockLabel();
    if (f->label.empty())
        f->label = lowerer_->nextFallbackBlockLabel();
    in.addBranchTarget(t->label);
    in.addBranchTarget(f->label);
    in.loc = lowerer_->curLoc;
    BasicBlock *block = lowerer_->context().current();
    assert(block && "emitCBr requires an active block");
    block->instructions.push_back(in);
    block->terminated = true;
}

/// @brief Emit a call instruction that returns a value.
/// @details Allocates a new temporary, records the callee identifier, and
///          appends a `Call` instruction so the result can flow to later
///          expressions.
/// @param ty Result type of the call.
/// @param callee Mangled name of the function to invoke.
/// @param args Argument values to pass to the callee.
/// @return Value referencing the call result.
CommonLowering::Value CommonLowering::emitCallRet(Type ty,
                                                  const std::string &callee,
                                                  const std::vector<Value> &args) {
    unsigned id = lowerer_->nextTempId();
    Instr in;
    in.result = id;
    in.op = Opcode::Call;
    in.type = ty;
    in.setDirectCallee(callee);
    in.operands = args;
    in.loc = lowerer_->curLoc;
    BasicBlock *block = lowerer_->context().current();
    assert(block && "emitCallRet requires an active block");
    block->instructions.push_back(in);
    return Value::temp(id);
}

/// @brief Emit a call instruction that ignores the return value.
/// @details Appends a void-typed `Call` instruction with the provided arguments
///          and callee name to the active block.
/// @param callee Mangled name of the function to invoke.
/// @param args Argument values to pass to the callee.
void CommonLowering::emitCall(const std::string &callee, const std::vector<Value> &args) {
    Instr in;
    in.op = Opcode::Call;
    in.type = Type(Type::Kind::Void);
    in.setDirectCallee(callee);
    in.operands = args;
    in.loc = lowerer_->curLoc;
    BasicBlock *block = lowerer_->context().current();
    assert(block && "emitCall requires an active block");
    block->instructions.push_back(in);
}

/// @brief Emits a value-returning indirect call.
/// @param ty Result type recorded in the indirect signature.
/// @param callee Function-pointer SSA value, stored as the first operand.
/// @param args Ordered call arguments appended after @p callee.
/// @return Temporary receiving the indirect call result.
CommonLowering::Value CommonLowering::emitCallIndirectRet(Type ty,
                                                          Value callee,
                                                          const std::vector<Value> &args) {
    unsigned id = lowerer_->nextTempId();
    Instr in;
    in.result = id;
    in.op = Opcode::CallIndirect;
    in.type = ty;
    in.setIndirectSignature(ty, {}, true);
    in.operands.reserve(1 + args.size());
    in.operands.push_back(callee);
    for (const auto &a : args)
        in.operands.push_back(a);
    in.loc = lowerer_->curLoc;
    BasicBlock *block = lowerer_->context().current();
    assert(block && "emitCallIndirectRet requires an active block");
    block->instructions.push_back(in);
    return Value::temp(id);
}

/// @brief Emits a void indirect call.
/// @param callee Function-pointer SSA value, stored as the first operand.
/// @param args Ordered call arguments appended after @p callee.
void CommonLowering::emitCallIndirect(Value callee, const std::vector<Value> &args) {
    Instr in;
    in.op = Opcode::CallIndirect;
    in.type = Type(Type::Kind::Void);
    in.setIndirectSignature(Type(Type::Kind::Void), {}, true);
    in.operands.reserve(1 + args.size());
    in.operands.push_back(callee);
    for (const auto &a : args)
        in.operands.push_back(a);
    in.loc = lowerer_->curLoc;
    BasicBlock *block = lowerer_->context().current();
    assert(block && "emitCallIndirect requires an active block");
    block->instructions.push_back(in);
}

/// @brief Materialise a string constant reference.
/// @details Emits a `ConstStr` instruction that refers to the global string
///          identified by @p globalName, producing a temporary that carries the
///          string type.
/// @param globalName Name of the global constant string symbol.
/// @return Value referencing the emitted constant.
CommonLowering::Value CommonLowering::emitConstStr(const std::string &globalName) {
    unsigned id = lowerer_->nextTempId();
    Instr in;
    in.result = id;
    in.op = Opcode::ConstStr;
    in.type = Type(Type::Kind::Str);
    in.operands.push_back(Value::global(globalName));
    in.loc = lowerer_->curLoc;
    BasicBlock *block = lowerer_->context().current();
    assert(block && "emitConstStr requires an active block");
    block->instructions.push_back(in);
    return Value::temp(id);
}

/// @brief Generate a unique basic block label using the active naming policy.
/// @details Prefers the @ref BlockNameGenerator when present so labels stay
///          human-readable for diagnostics; otherwise falls back to the mangler
///          on the owning @ref Lowerer.
/// @param base Human-readable stem for the label.
/// @return Unique label string safe to assign to a new block.
std::string CommonLowering::makeBlockLabel(std::string_view base) const {
    auto &ctx = lowerer_->context();
    if (auto *blockNamer = ctx.blockNames().namer())
        return blockNamer->generic(std::string(base));
    return lowerer_->mangler.block(std::string(base));
}

} // namespace il::frontends::basic::lower::common
