//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Defines the static lookup tables that describe verifier properties for each
// IL opcode.  Encapsulating the data here keeps the verification passes focused
// on logic while centralising metadata in one translation unit.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Opcode metadata tables consumed by the verifier.
/// @details Builds compile-time tables for operand arity, operand classes, and
///          side-effect information, exposing lookup helpers for use during
///          instruction checking.

#include "il/verify/VerifierTable.hpp"

#include <array>

namespace il::verify {
namespace {
using il::core::Opcode;
using Table = std::array<std::optional<OpProps>, static_cast<size_t>(Opcode::Count)>;

/// @brief Construct properties for a binary arithmetic-style opcode.
/// @details Convenience helper used when initialising the static table so each
///          entry records the arity, operand class, result class, and trapping
///          behaviour in a compact literal expression.
/// @param cls Shared type class required for both operands.
/// @param result Result type class.
/// @param canTrap Whether execution can trap.
/// @return Two-operand property record.
constexpr OpProps makeBinary(TypeClass cls, TypeClass result, bool canTrap) {
    return OpProps{2, cls, result, canTrap};
}

/// @brief Populate the static opcode properties table at program start.
/// @details Initialises all entries to `std::nullopt` then fills in the subset
///          of opcodes that currently have dedicated verification metadata.
///          Additional entries can be added here without modifying the runtime
///          lookup logic.
/// @return Fully initialized optional property table indexed by opcode.
constexpr Table buildTable() {
    Table table{};
    table.fill(std::nullopt);

    table[static_cast<size_t>(Opcode::IAddOvf)] =
        makeBinary(TypeClass::InstrType, TypeClass::InstrType, true);
    table[static_cast<size_t>(Opcode::ISubOvf)] =
        makeBinary(TypeClass::InstrType, TypeClass::InstrType, true);
    table[static_cast<size_t>(Opcode::IMulOvf)] =
        makeBinary(TypeClass::InstrType, TypeClass::InstrType, true);
    table[static_cast<size_t>(Opcode::SDiv)] = makeBinary(TypeClass::I64, TypeClass::I64, true);
    table[static_cast<size_t>(Opcode::UDiv)] = makeBinary(TypeClass::I64, TypeClass::I64, true);
    table[static_cast<size_t>(Opcode::SRem)] = makeBinary(TypeClass::I64, TypeClass::I64, true);
    table[static_cast<size_t>(Opcode::URem)] = makeBinary(TypeClass::I64, TypeClass::I64, true);
    table[static_cast<size_t>(Opcode::SDivChk0)] =
        makeBinary(TypeClass::InstrType, TypeClass::InstrType, true);
    table[static_cast<size_t>(Opcode::UDivChk0)] =
        makeBinary(TypeClass::InstrType, TypeClass::InstrType, true);
    table[static_cast<size_t>(Opcode::SRemChk0)] =
        makeBinary(TypeClass::InstrType, TypeClass::InstrType, true);
    table[static_cast<size_t>(Opcode::URemChk0)] =
        makeBinary(TypeClass::InstrType, TypeClass::InstrType, true);
    table[static_cast<size_t>(Opcode::And)] = makeBinary(TypeClass::I64, TypeClass::I64, false);
    table[static_cast<size_t>(Opcode::Or)] = makeBinary(TypeClass::I64, TypeClass::I64, false);
    table[static_cast<size_t>(Opcode::Xor)] = makeBinary(TypeClass::I64, TypeClass::I64, false);
    table[static_cast<size_t>(Opcode::Shl)] = makeBinary(TypeClass::I64, TypeClass::I64, false);
    table[static_cast<size_t>(Opcode::LShr)] = makeBinary(TypeClass::I64, TypeClass::I64, false);
    table[static_cast<size_t>(Opcode::AShr)] = makeBinary(TypeClass::I64, TypeClass::I64, false);
    table[static_cast<size_t>(Opcode::FAdd)] = makeBinary(TypeClass::F64, TypeClass::F64, false);
    table[static_cast<size_t>(Opcode::FSub)] = makeBinary(TypeClass::F64, TypeClass::F64, false);
    table[static_cast<size_t>(Opcode::FMul)] = makeBinary(TypeClass::F64, TypeClass::F64, false);
    table[static_cast<size_t>(Opcode::FDiv)] = makeBinary(TypeClass::F64, TypeClass::F64, false);

    return table;
}

const Table kTable = buildTable();

} // namespace

/// @brief Retrieve the optional property record for a given opcode.
/// @details Validates the index before accessing the static table to guard
///          against stale enumeration values.
/// @param opcode Opcode to query.
/// @return Property record for a covered opcode, or no value.
std::optional<OpProps> lookup(Opcode opcode) {
    const size_t index = static_cast<size_t>(opcode);
    if (index >= kTable.size())
        return std::nullopt;
    return kTable[index];
}

namespace {

/// @brief Translate a core type category into a verifier type class.
/// @details The verifier operates on a simplified view of types; this helper
///          performs the mapping while gracefully handling categories that have
///          no direct counterpart.
/// @param category Core opcode type category.
/// @return Matching verifier class, or `None` for unconstrained/dynamic input.
constexpr TypeClass mapCategory(il::core::TypeCategory category) {
    using il::core::TypeCategory;
    switch (category) {
        case TypeCategory::I1:
            return TypeClass::I1;
        case TypeCategory::I16:
            return TypeClass::I16;
        case TypeCategory::I32:
            return TypeClass::I32;
        case TypeCategory::I64:
            return TypeClass::I64;
        case TypeCategory::F64:
            return TypeClass::F64;
        case TypeCategory::Ptr:
            return TypeClass::Ptr;
        case TypeCategory::Str:
            return TypeClass::Str;
        case TypeCategory::Error:
            return TypeClass::Error;
        case TypeCategory::ResumeTok:
            return TypeClass::ResumeTok;
        case TypeCategory::InstrType:
            return TypeClass::InstrType;
        case TypeCategory::Void:
            return TypeClass::Void;
        case TypeCategory::None:
        case TypeCategory::Any:
        case TypeCategory::Dynamic:
            return TypeClass::None;
        default:
            return TypeClass::None;
    }
}

} // namespace

/// @brief Build a rich checking specification for the requested opcode.
/// @details Reads @ref il::core::OpcodeInfo and converts its operand descriptors
///          into @ref OpCheckSpec, capturing operand counts, expected types, and
///          side-effect flags.  Returns `std::nullopt` when the opcode is
///          outside the known range.
/// @param opcode Opcode to query.
/// @return Converted checking specification, or no value for an invalid index.
std::optional<OpCheckSpec> lookupSpec(Opcode opcode) {
    const size_t index = static_cast<size_t>(opcode);
    if (index >= il::core::kNumOpcodes)
        return std::nullopt;

    const auto &info = il::core::getOpcodeInfo(opcode);

    OpCheckSpec spec{};
    spec.numOperandsMin = info.numOperandsMin;
    spec.numOperandsMax = info.numOperandsMax;
    for (size_t i = 0; i < il::core::kMaxOperandCategories; ++i)
        spec.operandTypes[i] = mapCategory(info.operandTypes[i]);
    spec.result = mapCategory(info.resultType);
    spec.hasSideEffects = info.hasSideEffects;

    return spec;
}

/// @brief Determine whether an opcode is marked as having side effects.
/// @details Consults @ref lookupSpec first so override metadata can refine the
///          result, then falls back to @ref il::core::getOpcodeInfo when no
///          specialisation exists.
/// @param opcode Opcode to query.
/// @return `true` when metadata marks the opcode as side effecting.
bool hasSideEffects(Opcode opcode) {
    if (const auto spec = lookupSpec(opcode); spec.has_value())
        return spec->hasSideEffects;
    return il::core::getOpcodeInfo(opcode).hasSideEffects;
}

} // namespace il::verify
