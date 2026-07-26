//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the peephole optimisations that simplify short instruction
// sequences inside individual basic blocks.  The pass walks each function in a
// module, applies algebraic identity rules, and rewrites conditional branches
// whose predicate collapses to a constant value.  Transformations preserve the
// observable semantics of the module while eagerly removing redundant
// instructions so later passes operate on a smaller IR.
//
//===----------------------------------------------------------------------===//
//
// File Structure:
// ---------------
// This file is organized into the following sections:
//
// 1. Value Utilities
//    - isConstInt: Check if a value is an integer constant
//    - isConstEq: Check if a value equals a specific constant
//
// 2. Use Count Analysis
//    - UseCountMap: Type alias for temp ID to use count mapping
//    - buildUseCountMap: Build use counts for all temps in a function
//    - getUseCount: Query use count from precomputed map
//
// 3. Value Replacement
//    - replaceAll: Substitute all uses of a temp with a replacement value
//
// 4. Peephole Driver
//    - peephole(Module&): Main entry point
//      - Control flow simplification: CBr with known predicate -> Br
//      - Algebraic identity rules: Apply kRules table from header
//
// Rule Table (in Peephole.hpp):
// ----------------------------
// The kRules table defines algebraic identity and annihilation patterns:
// - Arithmetic identities: x + 0 = x, x * 1 = x, x - 0 = x
// - Arithmetic annihilations: x * 0 = 0, x - x = 0
// - Bitwise identities: x & -1 = x, x | 0 = x, x ^ 0 = x
// - Bitwise annihilations/reflexivity: x & 0 = 0, x ^ x = 0, x | x = x
// - Shift identities/annihilations: x << 0 = x, 0 << y = 0
// - Reflexive integer comparisons: icmp.eq x, x = true, scmp.lt x, x = false
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Peephole simplification utilities for IL modules.
/// @details Centralises the helpers that identify literal operands, count SSA
///          uses, and rewrite branch instructions so that the high-level
///          peephole driver can focus on pass orchestration.

#include "il/transform/Peephole.hpp"
#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Global.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Module.hpp"
#include "il/core/Type.hpp"
#include "il/core/Value.hpp"
#include "il/utils/Utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <unordered_map>
#include <unordered_set>

using namespace il::core;

namespace il::transform {

namespace {

//===----------------------------------------------------------------------===//
// Section 1: Value Utilities
//===----------------------------------------------------------------------===//

/// @brief Test whether a value is an integer constant and expose its payload.
///
/// The peephole rules only reason about literal integers. This helper centralises
/// the check and extraction so pattern matching can simply compare the numeric
/// payload in subsequent rules.
///
/// @param v      Candidate value operand.
/// @param out    Populated with the constant when the check succeeds.
/// @returns True when @p v is a @c ConstInt.
static bool isConstInt(const Value &v, long long &out) {
    if (v.kind == Value::Kind::ConstInt) {
        out = v.i64;
        return true;
    }
    return false;
}

/// @brief Determine whether an operand equals a specific integer literal.
///
/// The helper reuses @ref isConstInt to recognise literal integers and then
/// performs the comparison against @p target.  Centralising the logic allows the
/// peephole rule table to specify literal matches declaratively without in-line
/// conditionals at each call site.
///
/// @param v       Operand to classify.
/// @param target  Required constant value.
/// @returns True when the operand is an integer constant identical to
///          @p target.
static bool isConstEq(const Value &v, long long target) {
    long long c;
    return isConstInt(v, c) && c == target;
}

/// @brief Recognize positive powers of two and return the shift amount.
/// @param value Candidate signed integer constant.
/// @param shift Receives the base-two logarithm on success.
/// @return `true` when @p value is positive and has exactly one set bit.
static bool isPositivePowerOfTwo(long long value, unsigned &shift) {
    if (value <= 0)
        return false;
    const auto unsignedValue = static_cast<unsigned long long>(value);
    if ((unsignedValue & (unsignedValue - 1ULL)) != 0)
        return false;
    shift = 0;
    while ((1ULL << shift) != unsignedValue)
        ++shift;
    return true;
}

/// @brief Return the bit width of an integer type used by peephole rewrites.
/// @details The signed divide/remainder expansion must preserve the source
///          instruction width because arithmetic shift and mask constants are
///          width-sensitive for negative values.
/// @param kind IL type kind carried by the original instruction.
/// @return Bit width for supported integer types, or zero for unsupported types.
static unsigned integerBitWidth(Type::Kind kind) {
    switch (kind) {
        case Type::Kind::I16:
            return 16;
        case Type::Kind::I32:
            return 32;
        case Type::Kind::I64:
            return 64;
        default:
            return 0;
    }
}

/// @brief Compare floating constants without losing signed-zero identity.
/// @param value First floating-point value.
/// @param target Second floating-point value.
/// @return `true` for numerically equal nonzero values or zeros with equal signs.
static bool sameFloatConstant(double value, double target) {
    if (value != target)
        return false;
    if (value == 0.0)
        return std::signbit(value) == std::signbit(target);
    return true;
}

/// @brief Determine whether two operands are identical.
///
/// Peephole rules occasionally rely on reflexivity (e.g. @c xor x, x -> 0).
/// This helper checks equality across the supported operand kinds to keep the
/// rule application loop concise.
/// @param a First IL value to compare.
/// @param b Second IL value to compare.
/// @return `true` when both values have the same kind and payload.
static bool sameValue(const Value &a, const Value &b) {
    if (a.kind != b.kind)
        return false;
    switch (a.kind) {
        case Value::Kind::Temp:
            return a.id == b.id;
        case Value::Kind::ConstInt:
            return a.i64 == b.i64 && a.isBool == b.isBool;
        case Value::Kind::ConstFloat:
            return sameFloatConstant(a.f64, b.f64);
        case Value::Kind::ConstStr:
        case Value::Kind::GlobalAddr:
            return a.str == b.str;
        case Value::Kind::NullPtr:
            return true;
    }
    return false;
}

//===----------------------------------------------------------------------===//
// Section 2: Use Count Analysis
//===----------------------------------------------------------------------===//

/// @brief Map from temporary id to its use count within a function.
using UseCountMap = std::unordered_map<unsigned, size_t>;

/// @brief Build a use-count map for all temporaries in a function.
///
/// Walks every instruction in every block once, counting how many times each
/// temporary identifier appears as an operand. This precomputation avoids
/// repeated O(n) scans when checking use counts for multiple temporaries,
/// reducing the overall complexity from O(n*k) to O(n) where k is the number
/// of temporaries whose counts are queried.
///
/// @param f   Function to analyze.
/// @returns Map from temporary id to use count.
static UseCountMap buildUseCountMap(const Function &f) {
    UseCountMap counts;
    for (const auto &b : f.blocks) {
        for (const auto &in : b.instructions) {
            for (const auto &op : in.operands)
                if (op.kind == Value::Kind::Temp)
                    ++counts[op.id];
            for (const auto &argVec : in.brArgs)
                for (const auto &arg : argVec)
                    if (arg.kind == Value::Kind::Temp)
                        ++counts[arg.id];
        }
    }
    return counts;
}

/// @brief Look up the use count for a temporary from a precomputed map.
///
/// @param counts  Precomputed use-count map.
/// @param id      Temporary identifier to query.
/// @returns Number of uses, or 0 if not present.
static size_t getUseCount(const UseCountMap &counts, unsigned id) {
    auto it = counts.find(id);
    return it != counts.end() ? it->second : 0;
}

//===----------------------------------------------------------------------===//
// Section 3: Value Replacement
//===----------------------------------------------------------------------===//

/// @brief Substitute every use of a temporary with a replacement value.
///
/// Arithmetic identity rules forward an existing operand in place of the
/// computed result. Once a rule matches, this helper rewrites all uses before
/// the defining instruction is removed, preserving SSA-style data flow without
/// altering block structure.
///
/// @param f   Function whose operands should be updated.
/// @param id  Temporary identifier to replace.
/// @param v   Replacement value propagated to all uses.
static void replaceAll(Function &f, unsigned id, const Value &v) {
    for (auto &b : f.blocks)
        for (auto &in : b.instructions) {
            for (auto &op : in.operands)
                if (op.kind == Value::Kind::Temp && op.id == id)
                    op = v;
            for (auto &argVec : in.brArgs)
                for (auto &arg : argVec)
                    if (arg.kind == Value::Kind::Temp && arg.id == id)
                        arg = v;
        }
}

/// @brief Test whether a value is a reference to a particular SSA temporary.
/// @param value Operand or branch argument to inspect.
/// @param id Temporary id to match.
/// @return `true` only for a temporary value with id @p id.
static bool valueUsesTemp(const Value &value, unsigned id) {
    return value.kind == Value::Kind::Temp && value.id == id;
}

/// @brief Test whether an instruction uses a temporary in any value position.
/// @param instr Instruction whose operands and branch bundles are scanned.
/// @param id Temporary id to locate.
/// @return `true` on the first matching reference.
static bool instructionUsesTemp(const Instr &instr, unsigned id) {
    for (const auto &op : instr.operands)
        if (valueUsesTemp(op, id))
            return true;
    for (const auto &argVec : instr.brArgs)
        for (const auto &arg : argVec)
            if (valueUsesTemp(arg, id))
                return true;
    return false;
}

/// @brief Check whether every use of @p id is after @p defIdx in @p block.
///
/// @details Operand-forwarding peepholes replace a result with one of the
///          defining instruction's operands. Keeping these rewrites local avoids
///          introducing cross-edge branch arguments whose replacement value is
///          not available on that predecessor edge.
/// @param f Function containing every possible use.
/// @param block Block containing the definition.
/// @param defIdx Instruction index of the definition.
/// @param id Result id whose uses are checked.
/// @return `true` when every use is later in the same block.
static bool allUsesLocalAfter(const Function &f,
                              const BasicBlock &block,
                              size_t defIdx,
                              unsigned id) {
    for (const auto &candidate : f.blocks) {
        for (size_t idx = 0; idx < candidate.instructions.size(); ++idx) {
            if (!instructionUsesTemp(candidate.instructions[idx], id))
                continue;
            if (&candidate != &block || idx <= defIdx)
                return false;
        }
    }
    return true;
}

/// @brief Replace later uses of a temporary within one block.
/// @param block Block whose suffix is rewritten.
/// @param defIdx Index before the first instruction to examine.
/// @param id Temporary id to replace.
/// @param v Replacement value known to be available throughout the suffix.
static void replaceLocalUsesAfter(BasicBlock &block, size_t defIdx, unsigned id, const Value &v) {
    for (size_t idx = defIdx + 1; idx < block.instructions.size(); ++idx) {
        Instr &instr = block.instructions[idx];
        for (auto &op : instr.operands)
            if (valueUsesTemp(op, id))
                op = v;
        for (auto &argVec : instr.brArgs)
            for (auto &arg : argVec)
                if (valueUsesTemp(arg, id))
                    arg = v;
    }
}

//===----------------------------------------------------------------------===//
// Comparison Constant Folding (for CBr simplification)
//===----------------------------------------------------------------------===//

/// @brief Evaluate a comparison opcode with two constant operands.
/// @param op  The comparison opcode.
/// @param l   Left-hand side constant.
/// @param r   Right-hand side constant.
/// @param out Output boolean result if comparison is foldable.
/// @returns True if the comparison was evaluated, false if opcode not supported.
static bool evaluateComparison(Opcode op, long long l, long long r, long long &out) {
    auto ul = static_cast<unsigned long long>(l);
    auto ur = static_cast<unsigned long long>(r);
    switch (op) {
        case Opcode::ICmpEq:
            out = (l == r);
            return true;
        case Opcode::ICmpNe:
            out = (l != r);
            return true;
        case Opcode::SCmpLT:
            out = (l < r);
            return true;
        case Opcode::SCmpLE:
            out = (l <= r);
            return true;
        case Opcode::SCmpGT:
            out = (l > r);
            return true;
        case Opcode::SCmpGE:
            out = (l >= r);
            return true;
        case Opcode::UCmpLT:
            out = (ul < ur);
            return true;
        case Opcode::UCmpLE:
            out = (ul <= ur);
            return true;
        case Opcode::UCmpGT:
            out = (ul > ur);
            return true;
        case Opcode::UCmpGE:
            out = (ul >= ur);
            return true;
        default:
            return false;
    }
}

/// @brief Evaluate a floating-point comparison with two literal operands.
/// @param op Comparison opcode to interpret.
/// @param l Left operand.
/// @param r Right operand.
/// @param out Receives zero or one when @p op is supported.
/// @return `true` for a recognized ordered comparison opcode.
static bool evaluateFloatComparison(Opcode op, double l, double r, long long &out) {
    switch (op) {
        case Opcode::FCmpEQ:
            out = (l == r);
            return true;
        case Opcode::FCmpNE:
            out = (l != r);
            return true;
        case Opcode::FCmpLT:
            out = (l < r);
            return true;
        case Opcode::FCmpLE:
            out = (l <= r);
            return true;
        case Opcode::FCmpGT:
            out = (l > r);
            return true;
        case Opcode::FCmpGE:
            out = (l >= r);
            return true;
        default:
            return false;
    }
}

/// @brief Query whether peephole rule tracing was requested at process startup.
/// @return Cached presence of the `ZANNA_PEEPHOLE_TRACE` environment variable.
static bool traceEnabled() {
    static const bool enabled = std::getenv("ZANNA_PEEPHOLE_TRACE") != nullptr;
    return enabled;
}

/// @brief Check if a value is a float constant with the expected value.
/// @param v Candidate literal.
/// @param target Expected payload, including signed-zero identity.
/// @return `true` when @p v is the requested floating-point constant.
static bool isConstFloatEq(const Value &v, double target) {
    if (v.kind == Value::Kind::ConstFloat) {
        return sameFloatConstant(v.f64, target);
    }
    return false;
}

/// @brief Determine whether @p in matches @p rule and compute the replacement.
///
/// @param rule Peephole rule to evaluate.
/// @param in Instruction to inspect (must be binary with a result).
/// @param out Receives the replacement value on match.
/// @returns True when the rule matched and @p out is valid.
static bool applyRule(const Rule &rule, const Instr &in, Value &out) {
    if (in.op != rule.match.op || in.operands.size() != 2)
        return false;

    switch (rule.match.kind) {
        case Match::Kind::ConstOperand:
            if (!isConstEq(in.operands[rule.match.constIdx], rule.match.value))
                return false;
            break;
        case Match::Kind::ConstFloatOperand:
            if (!isConstFloatEq(in.operands[rule.match.constIdx], rule.match.floatValue))
                return false;
            break;
        case Match::Kind::SameOperands:
            if (!sameValue(in.operands[0], in.operands[1]))
                return false;
            break;
    }

    switch (rule.repl.kind) {
        case Replace::Kind::Operand:
            out = in.operands[rule.repl.operandIdx];
            return true;
        case Replace::Kind::Const:
            out = rule.repl.isBool ? Value::constBool(rule.repl.constValue != 0)
                                   : Value::constInt(rule.repl.constValue);
            return true;
        case Replace::Kind::ConstFloat:
            out = Value::constFloat(rule.repl.floatConstValue);
            return true;
    }
    return false;
}

/// @brief Rewrite unsigned division/remainder by a constant power of two.
/// @details Checked variants are safe to rewrite because the matched divisor is
///          a non-zero constant. Signed division is deliberately excluded: it
///          rounds toward zero, while arithmetic shifts round toward -infinity.
/// @param in Candidate division or remainder instruction, rewritten in place.
/// @return `true` when the divisor is a power of two greater than one.
static bool tryRewriteUnsignedPowerOfTwoDivRem(Instr &in) {
    if (in.operands.size() != 2)
        return false;

    const bool isDiv = in.op == Opcode::UDiv || in.op == Opcode::UDivChk0;
    const bool isRem = in.op == Opcode::URem || in.op == Opcode::URemChk0;
    if (!isDiv && !isRem)
        return false;

    long long divisor = 0;
    unsigned shift = 0;
    if (!isConstInt(in.operands[1], divisor) || !isPositivePowerOfTwo(divisor, shift) || shift == 0)
        return false;

    if (isDiv) {
        in.op = Opcode::LShr;
        in.operands[1] = Value::constInt(static_cast<long long>(shift));
    } else {
        in.op = Opcode::And;
        in.operands[1] = Value::constInt(divisor - 1);
    }
    return true;
}

/// @brief Expand signed division/remainder by a positive power of two.
/// @details Signed division rounds toward zero, so plain arithmetic shift is
///          wrong for negative dividends. The expansion applies the standard
///          sign-bias correction before the final shift/mask:
///
///   q = (x + ((x >> 63) & (d - 1))) >> log2(d)
///   r = x - (q << log2(d))
///
/// The remainder form computes the rounded multiple with a mask to avoid
/// needing a multiply or left shift. Checked add/sub are used because they are
/// verifier-legal before range cleanup; the correction is mathematically safe.
/// @param function Function whose value-name table is extended for fresh ids.
/// @param block Block containing the instruction to expand.
/// @param idx Instruction index, advanced to the final replacement instruction.
/// @param nextId Fresh-id counter updated for all synthesized intermediates.
/// @return `true` after replacing a supported signed division or remainder.
static bool tryExpandSignedPowerOfTwoDivRem(Function &function,
                                            BasicBlock &block,
                                            size_t &idx,
                                            unsigned &nextId) {
    if (idx >= block.instructions.size())
        return false;

    Instr &in = block.instructions[idx];
    if (!in.result || in.operands.size() != 2)
        return false;

    const bool isDiv = in.op == Opcode::SDiv || in.op == Opcode::SDivChk0;
    const bool isRem = in.op == Opcode::SRem || in.op == Opcode::SRemChk0;
    if (!isDiv && !isRem)
        return false;

    long long divisor = 0;
    unsigned shift = 0;
    if (!isConstInt(in.operands[1], divisor) || !isPositivePowerOfTwo(divisor, shift) || shift == 0)
        return false;

    const unsigned bitWidth = integerBitWidth(in.type.kind);
    if (bitWidth == 0)
        return false;

    const Value dividend = in.operands[0];
    const unsigned originalResult = *in.result;
    const Type resultType = in.type;

    const unsigned signId = nextId++;
    const unsigned biasId = nextId++;
    const unsigned biasedId = nextId++;

    Instr sign;
    sign.op = Opcode::AShr;
    sign.result = signId;
    sign.type = resultType;
    sign.operands = {dividend, Value::constInt(static_cast<long long>(bitWidth - 1))};

    Instr bias;
    bias.op = Opcode::And;
    bias.result = biasId;
    bias.type = resultType;
    bias.operands = {Value::temp(signId), Value::constInt(divisor - 1)};

    Instr biased;
    biased.op = Opcode::IAddOvf;
    biased.result = biasedId;
    biased.type = resultType;
    biased.operands = {dividend, Value::temp(biasId)};

    std::vector<Instr> replacement;
    replacement.reserve(isDiv ? 4 : 5);
    replacement.push_back(std::move(sign));
    replacement.push_back(std::move(bias));
    replacement.push_back(std::move(biased));

    if (isDiv) {
        Instr quotient;
        quotient.op = Opcode::AShr;
        quotient.result = originalResult;
        quotient.type = resultType;
        quotient.operands = {Value::temp(biasedId), Value::constInt(static_cast<long long>(shift))};
        replacement.push_back(std::move(quotient));
    } else {
        const unsigned roundedId = nextId++;

        Instr rounded;
        rounded.op = Opcode::And;
        rounded.result = roundedId;
        rounded.type = resultType;
        rounded.operands = {Value::temp(biasedId), Value::constInt(~(divisor - 1))};

        Instr rem;
        rem.op = Opcode::ISubOvf;
        rem.result = originalResult;
        rem.type = resultType;
        rem.operands = {dividend, Value::temp(roundedId)};

        replacement.push_back(std::move(rounded));
        replacement.push_back(std::move(rem));
    }

    auto insertPos = block.instructions.begin() + static_cast<std::ptrdiff_t>(idx);
    block.instructions.erase(insertPos);
    block.instructions.insert(block.instructions.begin() + static_cast<std::ptrdiff_t>(idx),
                              std::make_move_iterator(replacement.begin()),
                              std::make_move_iterator(replacement.end()));

    if (function.valueNames.size() < nextId)
        function.valueNames.resize(nextId);
    idx += replacement.size() - 1;
    return true;
}

/// @brief Look up a string-typed global by name.
/// @param module Module whose global table is searched.
/// @param name Exact global identifier.
/// @return Borrowed matching global, or null for an absent/non-string symbol.
const Global *findStringGlobal(const Module &module, const std::string &name) {
    for (const auto &global : module.globals)
        if (global.name == name && global.type.kind == Type::Kind::Str)
            return &global;
    return nullptr;
}

/// @brief Intern a folded string literal as a module global.
/// @param module Module searched and possibly extended.
/// @param value Literal initializer to reuse or add.
/// @return Name of an existing matching string global or a fresh private-style name.
std::string findOrCreateStringGlobal(Module &module, const std::string &value) {
    for (const auto &global : module.globals) {
        if (global.type.kind == Type::Kind::Str && global.init == value)
            return global.name;
    }

    std::unordered_set<std::string> usedNames;
    usedNames.reserve(module.globals.size() + module.functions.size() + module.externs.size());
    for (const auto &global : module.globals)
        usedNames.insert(global.name);
    for (const auto &function : module.functions)
        usedNames.insert(function.name);
    for (const auto &ext : module.externs)
        usedNames.insert(ext.name);

    std::string name;
    unsigned suffix = 0;
    do {
        name = ".Lopt.concat." + std::to_string(suffix++);
    } while (usedNames.contains(name));

    Global global;
    global.name = name;
    global.type = Type(Type::Kind::Str);
    global.init = value;
    global.isConst = true;
    global.hasInitializer = true;
    global.nameSymbol = module.internIdentifier(name);
    module.globals.push_back(std::move(global));
    return name;
}

/// @brief Local `const.str` definition and its referenced global.
struct ConstStrDef {
    /// Instruction index of the definition.
    size_t index = 0;
    /// Name supplied by its global-address operand.
    std::string globalName;
};

/// @brief Find the local constant-string definition of a temporary.
/// @param block Block searched backward from @p beforeIdx.
/// @param beforeIdx Exclusive upper instruction index.
/// @param value Temporary expected to name a `const.str` result.
/// @return Definition location and global name, or `std::nullopt`.
std::optional<ConstStrDef> findLocalConstStrDef(const BasicBlock &block,
                                                size_t beforeIdx,
                                                const Value &value) {
    if (value.kind != Value::Kind::Temp)
        return std::nullopt;
    for (size_t i = beforeIdx; i > 0; --i) {
        const size_t idx = i - 1;
        const Instr &candidate = block.instructions[idx];
        if (!candidate.result || *candidate.result != value.id)
            continue;
        if (candidate.op != Opcode::ConstStr || candidate.operands.size() != 1 ||
            candidate.operands[0].kind != Value::Kind::GlobalAddr) {
            return std::nullopt;
        }
        return ConstStrDef{idx, candidate.operands[0].str};
    }
    return std::nullopt;
}

/// @brief Fold a two-literal runtime string concatenation into one constant.
/// @param module Module providing and receiving string globals.
/// @param block Block containing both literal definitions and the concat call.
/// @param idx Concat instruction index, adjusted after deleting the definitions.
/// @param useCounts Function use counts proving both temporary strings are single-use.
/// @return `true` after replacing the call with `const.str`.
bool tryFoldLiteralConcat(Module &module,
                          BasicBlock &block,
                          size_t &idx,
                          const UseCountMap &useCounts) {
    if (idx >= block.instructions.size())
        return false;

    Instr &call = block.instructions[idx];
    const bool isStringConcat =
        call.callee == "rt_str_concat" || call.callee == "Zanna.String.Concat";
    if (call.op != Opcode::Call || !isStringConcat || !call.result || call.operands.size() != 2) {
        return false;
    }

    const Value &lhs = call.operands[0];
    const Value &rhs = call.operands[1];
    if (lhs.kind != Value::Kind::Temp || rhs.kind != Value::Kind::Temp)
        return false;
    if (getUseCount(useCounts, lhs.id) != 1 || getUseCount(useCounts, rhs.id) != 1)
        return false;

    auto lhsDef = findLocalConstStrDef(block, idx, lhs);
    auto rhsDef = findLocalConstStrDef(block, idx, rhs);
    if (!lhsDef || !rhsDef || lhsDef->index == rhsDef->index)
        return false;

    const Global *lhsGlobal = findStringGlobal(module, lhsDef->globalName);
    const Global *rhsGlobal = findStringGlobal(module, rhsDef->globalName);
    if (!lhsGlobal || !rhsGlobal)
        return false;

    const std::string foldedName =
        findOrCreateStringGlobal(module, lhsGlobal->init + rhsGlobal->init);

    Instr replacement;
    replacement.op = Opcode::ConstStr;
    replacement.result = call.result;
    replacement.type = Type(Type::Kind::Str);
    replacement.operands = {Value::global(foldedName)};

    std::array<size_t, 2> eraseIndices{lhsDef->index, rhsDef->index};
    std::sort(eraseIndices.begin(), eraseIndices.end(), std::greater<size_t>{});

    size_t adjustedIdx = idx;
    for (size_t eraseIdx : eraseIndices) {
        block.instructions.erase(block.instructions.begin() +
                                 static_cast<std::ptrdiff_t>(eraseIdx));
        if (eraseIdx < adjustedIdx)
            --adjustedIdx;
    }

    block.instructions[adjustedIdx] = std::move(replacement);
    idx = adjustedIdx;
    return true;
}

} // namespace

//===----------------------------------------------------------------------===//
// Section 4: Peephole Driver
//===----------------------------------------------------------------------===//

/// @brief Apply local simplifications to all functions in a module.
///
/// The pass performs two kinds of optimisation:
///  - Apply algebraic identity rules from @c kRules, forwarding constant-folded
///    operands and erasing the now-dead producer instruction.
///  - Simplify conditional branches whose predicate collapses to a known boolean
///    value, rewriting them into unconditional jumps.
///
/// When a branch is rewritten, the routine also prunes the untaken successor's
/// block parameters by trimming @c brArgs so the surviving edge keeps its
/// argument arity in sync with the callee block. Additionally, a branch-condition
/// definition is erased when the predicate was produced in the same block and
/// had a single use, ensuring we do not leave dead instructions behind. The
/// implementation intentionally limits itself to integer comparisons with
/// literal operands and does not chase values across blocks or through
/// non-literal arithmetic.
/// @param m Module whose function bodies and string globals are simplified.
static void runPeephole(Module &m) {
    const bool trace = traceEnabled();
    for (auto &f : m.functions) {
        unsigned nextId = zanna::il::nextTempId(f);
        UseCountMap useCounts = buildUseCountMap(f);
        /// Recompute use counts after each local mutation invalidates the snapshot.
        auto refreshUseCounts = [&]() { useCounts = buildUseCountMap(f); };

        for (auto &b : f.blocks) {
            for (size_t i = 0; i < b.instructions.size(); ++i) {
                Instr &in = b.instructions[i];

                //===----------------------------------------------------------===//
                // Control Flow Simplification: CBr -> Br
                //===----------------------------------------------------------===//
                // Simplify conditional branches where:
                // 1. Both targets are the same label (trivial case)
                // 2. The condition is a constant
                // 3. The condition is defined by a comparison with constant operands
                //===----------------------------------------------------------===//
                if (in.op == Opcode::CBr) {
                    // Case 1: Both targets identical -> unconditional branch.
                    // Only safe when both edge argument sets are identical;
                    // if they differ (e.g., after block merging), leave the CBr
                    // for SimplifyCFG to handle with proper phi insertion.
                    if (in.labels.size() == 2 && in.labels[0] == in.labels[1]) {
                        bool argsMatch = (in.brArgs.size() <= 1);
                        if (!argsMatch && in.brArgs.size() == 2 &&
                            in.brArgs[0].size() == in.brArgs[1].size()) {
                            argsMatch = true;
                            for (size_t a = 0; a < in.brArgs[0].size(); ++a)
                                if (!sameValue(in.brArgs[0][a], in.brArgs[1][a])) {
                                    argsMatch = false;
                                    break;
                                }
                        }
                        if (argsMatch) {
                            in.op = Opcode::Br;
                            in.operands.clear();
                            std::vector<std::vector<Value>> args;
                            if (!in.brArgs.empty())
                                args.push_back(in.brArgs.front());
                            in.setBranchTargets({in.labels[0]}, std::move(args));
                            refreshUseCounts();
                        }
                        continue;
                    }

                    // Try to determine the branch condition value
                    if (in.operands.empty())
                        continue;
                    long long v;
                    bool known = false;
                    size_t defIdx = static_cast<size_t>(-1);
                    size_t uses = 0;

                    // Case 2: Condition is a constant
                    if (isConstInt(in.operands[0], v)) {
                        known = true;
                    }
                    // Case 3: Condition is defined by a comparison in this block
                    else if (in.operands[0].kind == Value::Kind::Temp) {
                        unsigned id = in.operands[0].id;
                        uses = getUseCount(useCounts, id);

                        // Search backwards for the defining instruction
                        for (size_t j = 0; j < i; ++j) {
                            Instr &def = b.instructions[j];
                            if (def.result && *def.result == id && def.operands.size() == 2) {
                                long long l, r;
                                if (isConstInt(def.operands[0], l) &&
                                    isConstInt(def.operands[1], r)) {
                                    if (evaluateComparison(def.op, l, r, v)) {
                                        known = true;
                                        defIdx = j;
                                    }
                                }
                                // Also try float constant comparisons
                                if (!known && def.operands[0].kind == Value::Kind::ConstFloat &&
                                    def.operands[1].kind == Value::Kind::ConstFloat) {
                                    if (evaluateFloatComparison(
                                            def.op, def.operands[0].f64, def.operands[1].f64, v)) {
                                        known = true;
                                        defIdx = j;
                                    }
                                }
                            }
                            if (known)
                                break;
                        }
                    }

                    // Rewrite CBr to Br if condition is known
                    if (known) {
                        in.op = Opcode::Br;
                        const bool takeTrue = (v != 0);
                        std::string target = takeTrue ? in.labels[0] : in.labels[1];

                        // Preserve the correct branch arguments
                        std::vector<std::vector<Value>> args;
                        if (!in.brArgs.empty()) {
                            if (takeTrue) {
                                args.push_back(in.brArgs.front());
                            } else if (in.brArgs.size() > 1) {
                                args.push_back(in.brArgs[1]);
                            }
                        }
                        in.setBranchTargets({std::move(target)}, std::move(args));
                        in.operands.clear();

                        // Remove the dead comparison if it was single-use
                        if (defIdx != static_cast<size_t>(-1) && uses == 1) {
                            b.instructions.erase(b.instructions.begin() + defIdx);
                            --i;
                        }
                        refreshUseCounts();
                    }
                    continue;
                }

                if (tryFoldLiteralConcat(m, b, i, useCounts)) {
                    if (trace)
                        std::cerr << "[peephole] folded literal concat (func " << f.name << ")\n";
                    refreshUseCounts();
                    continue;
                }

                //===----------------------------------------------------------===//
                // Algebraic Identity Rules
                //===----------------------------------------------------------===//
                // Apply rules from kRules table:
                // - Match: opcode with either a specific constant operand or
                //   equal operands (reflexive simplifications).
                // - Action: Forward an operand or replace with a literal.
                //===----------------------------------------------------------===//
                if (!in.result)
                    continue;

                if (tryExpandSignedPowerOfTwoDivRem(f, b, i, nextId)) {
                    if (trace)
                        std::cerr << "[peephole] signed power-of-two div/rem (func " << f.name
                                  << ")\n";
                    refreshUseCounts();
                    continue;
                }

                if (tryRewriteUnsignedPowerOfTwoDivRem(in)) {
                    if (trace)
                        std::cerr << "[peephole] unsigned power-of-two div/rem in %" << *in.result
                                  << " (func " << f.name << ")\n";
                    refreshUseCounts();
                    continue;
                }

                Value repl{};
                bool match = false;
                for (const auto &r : kRules) {
                    if (applyRule(r, in, repl)) {
                        match = true;
                        if (trace && in.result)
                            std::cerr << "[peephole] rule " << r.name << " in %" << *in.result
                                      << " (func " << f.name << ")\n";
                        break;
                    }
                }

                if (match) {
                    if (repl.kind == Value::Kind::Temp) {
                        if (!allUsesLocalAfter(f, b, i, *in.result))
                            continue;
                        replaceLocalUsesAfter(b, i, *in.result, repl);
                    } else {
                        replaceAll(f, *in.result, repl);
                    }
                    b.instructions.erase(b.instructions.begin() + i);
                    --i;
                    refreshUseCounts();
                }
            }
        }
    }
}

/// @brief Run local peephole simplification and refresh identifier ownership.
/// @param m Module optimized in place.
void peephole(Module &m) {
    runPeephole(m);
    m.internOwnedIdentifiers();
}

} // namespace il::transform
