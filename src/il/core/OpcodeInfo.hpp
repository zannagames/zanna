//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/core/OpcodeInfo.hpp
// Purpose: Declares metadata describing IL opcode signatures and behaviours.
// Key invariants: Table entries cover every Opcode enumerator exactly once.
// Ownership/Lifetime: Metadata is static storage duration and read-only.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares canonical signature, parser, effect, and VM metadata for opcodes.
 *
 * @details Every valid `Opcode` indexes exactly one immutable `OpcodeInfo`
 *          record describing result arity, operand constraints, control-flow
 *          shape, parser strategy, and interpreter dispatch. Shared helpers
 *          expose that table and conservatively classify memory behavior for
 *          analyses and transformations.
 */

#pragma once

#include "il/core/Opcode.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace il::core {

/// @brief Sentinel value representing variadic operand arity.
inline constexpr uint8_t kVariadicOperandCount = std::numeric_limits<uint8_t>::max();

/// @brief Result arity expectation for an opcode.
enum class ResultArity : uint8_t {
    None = 0,       ///< Instruction never produces a result.
    One = 1,        ///< Instruction must produce exactly one result.
    Optional = 0xFF ///< Instruction may omit or provide a result.
};

/// @brief Type category requirement for operands or results.
enum class TypeCategory : uint8_t {
    None,      ///< Unused slot or no constraint.
    Void,      ///< Void type (primarily for annotations).
    I1,        ///< Boolean integer type.
    I16,       ///< 16-bit integer type.
    I32,       ///< 32-bit integer type.
    I64,       ///< 64-bit integer type.
    F64,       ///< 64-bit floating point type.
    Ptr,       ///< Pointer type.
    Str,       ///< Runtime string type.
    Error,     ///< Opaque VM error record.
    ResumeTok, ///< Opaque resume token provided to handlers.
    Any,       ///< No specific type requirement.
    InstrType, ///< Type derived from the instruction's @c type field.
    Dynamic    ///< Type derived from external context (e.g., call signature).
};

/// @brief Coarse classification of an opcode's interaction with memory.
/// @details Designed for analyses that need conservative read/write flags
///          without performing full alias modelling.  Unknown values should be
///          treated as both reading and writing memory by consumers.
enum class MemoryEffects : uint8_t {
    None,      ///< Instruction is known to avoid memory reads and writes.
    Read,      ///< Instruction only reads memory.
    Write,     ///< Instruction only writes memory.
    ReadWrite, ///< Instruction may both read and write memory.
    Unknown    ///< Insufficient information; assume reads and writes occur.
};

/// @brief Identifier describing VM dispatch strategy for an opcode.
enum class VMDispatch : uint8_t {
    None, ///< No interpreter handler implemented yet.
    Alloca,
    Load,
    Store,
    GEP,
    Add,
    Sub,
    ISub,
    Mul,
    IAddOvf,
    ISubOvf,
    IMulOvf,
    SDiv,
    UDiv,
    SRem,
    URem,
    SDivChk0,
    UDivChk0,
    SRemChk0,
    URemChk0,
    IdxChk,
    And,
    Or,
    Xor,
    Shl,
    LShr,
    AShr,
    FAdd,
    FSub,
    FMul,
    FDiv,
    ICmpEq,
    ICmpNe,
    SCmpGT,
    SCmpLT,
    SCmpLE,
    SCmpGE,
    UCmpLT,
    UCmpLE,
    UCmpGT,
    UCmpGE,
    FCmpEQ,
    FCmpNE,
    FCmpGT,
    FCmpLT,
    FCmpLE,
    FCmpGE,
    SwitchI32,
    Br,
    CBr,
    Ret,
    AddrOf,
    ConstStr,
    GAddr,
    ConstNull,
    Call,
    CallIndirect,
    Sitofp,
    Fptosi,
    CastFpToSiRteChk,
    CastFpToUiRteChk,
    CastSiNarrowChk,
    CastUiNarrowChk,
    CastSiToFp,
    CastUiToFp,
    TruncOrZext1,
    ErrGet,
    Trap,
    TrapFromErr,
    TrapErrMake,
    TrapKindRead,
    EhPush,
    EhPop,
    ResumeSame,
    ResumeNext,
    ResumeLabel,
    EhEntry,
    FCmpOrd,  ///< Floating-point ordered comparison (neither is NaN).
    FCmpUno,  ///< Floating-point unordered comparison (either is NaN).
    ConstF64, ///< Load a constant 64-bit float.
    Select,   ///< Ternary select between two same-typed values.
    Count     ///< Sentinel enumerating the number of dispatch kinds.
};

/// @brief Maximum number of operand categories stored per opcode.
inline constexpr size_t kMaxOperandCategories = 3;

/// @brief Describes how the textual parser should interpret an operand slot.
enum class OperandParseKind : uint8_t {
    None,          ///< No token expected in this slot.
    Value,         ///< Parse a general value operand.
    TypeImmediate, ///< Parse a type literal influencing the instruction type.
    BranchTarget,  ///< Parse a successor label with optional arguments.
    Call,          ///< Parse call-style callee and argument list syntax.
    CallIndirect,  ///< Parse call.indirect %fnPtr(%arg1, ...) syntax.
    Switch         ///< Parse switch scrutinee/default/case syntax.
};

/// @brief Maximum number of parser descriptors stored per opcode.
inline constexpr size_t kMaxOperandParseEntries = 4;

/// @brief Declarative description of how to parse an opcode's tokens.
struct OperandParseSpec {
    OperandParseKind kind{OperandParseKind::None}; ///< Kind of token expected at this position.
    const char *role{nullptr}; ///< Human-readable role used for diagnostics (optional).
};

/// @brief Static description of an opcode signature and behaviour.
struct OpcodeInfo {
    const char *name{""}; ///< Canonical mnemonic.
    ResultArity resultArity{ResultArity::None}; ///< Expected result arity.
    TypeCategory resultType{TypeCategory::None}; ///< Result type constraint, if any.
    uint8_t numOperandsMin{0}; ///< Minimum operand count.
    uint8_t numOperandsMax{0}; ///< Maximum operand count or kVariadicOperandCount.
    std::array<TypeCategory, kMaxOperandCategories> operandTypes{}; ///< Operand constraints.
    bool hasSideEffects{false}; ///< Instruction mutates state or control flow.
    uint8_t numSuccessors{0};   ///< Number of successor labels required.
    bool isTerminator{false};   ///< Instruction terminates a block.
    VMDispatch vmDispatch{VMDispatch::None}; ///< Interpreter dispatch category.
    std::array<OperandParseSpec, kMaxOperandParseEntries> parse{}; ///< Textual parsing recipe.
};

/// @brief Metadata table indexed by @c Opcode enumerators.
extern const std::array<OpcodeInfo, kNumOpcodes> kOpcodeTable;

/// @brief Access metadata for a specific opcode.
/// @param op Opcode to query.
/// @return Reference to the metadata entry for @p op.
const OpcodeInfo &getOpcodeInfo(Opcode op);

/// @brief Enumerate all opcodes defined by the IL in declaration order.
/// @return Const reference to a cached vector containing every opcode exactly once.
const std::vector<Opcode> &all_opcodes();

/// @brief Retrieve the conservative memory interaction classification.
/// @param op Opcode to query.
/// @return MemoryEffects value describing @p op's memory behaviour.
MemoryEffects memoryEffects(Opcode op) noexcept;

/// @brief Determine whether an opcode is known to read from memory.
/// @param op Opcode to query.
/// @return True if @p op may read memory.
inline bool hasMemoryRead(Opcode op) noexcept {
    using il::core::MemoryEffects;
    const auto me = memoryEffects(op);
    return me == MemoryEffects::Read || me == MemoryEffects::ReadWrite ||
           me == MemoryEffects::Unknown;
}

/// @brief Determine whether an opcode is known to write to memory.
/// @param op Opcode to query.
/// @return True if @p op may write memory.
inline bool hasMemoryWrite(Opcode op) noexcept {
    using il::core::MemoryEffects;
    const auto me = memoryEffects(op);
    return me == MemoryEffects::Write || me == MemoryEffects::ReadWrite ||
           me == MemoryEffects::Unknown;
}

/// @brief Retrieve the canonical mnemonic string for an opcode.
/// @param op Opcode to translate into text.
/// @return Lowercase mnemonic defined by the IL spec, empty if invalid.
std::string opcode_mnemonic(Opcode op);

/// @brief Determine whether @p value denotes a variadic operand upper bound.
/// @param value Encoded operand-count field.
/// @return True when @p value equals @ref kVariadicOperandCount.
bool isVariadicOperandCount(uint8_t value);

/// @brief Determine whether @p value denotes a variadic successor count.
/// @param value Encoded successor-count field.
/// @return True when @p value uses the shared variadic-count sentinel.
bool isVariadicSuccessorCount(uint8_t value);

} // namespace il::core
