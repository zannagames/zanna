//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/bytecode/Bytecode.hpp
// Purpose: Bytecode instruction format, opcode definitions, and runtime value type.
// Key invariants: All instructions are 32-bit fixed-width (or 64-bit extended).
//                 Opcodes are grouped by functional category for cache-friendly dispatch.
//                 BCSlot is exactly 8 bytes (static_assert enforced).
// Ownership: Part of the bytecode subsystem; no external dependencies.
// Lifetime: Constants and inline helpers are header-only; opcodeName() is defined
//           in the corresponding .cpp translation unit.
// Links: docs/internals/architecture.md, BytecodeCompiler.hpp, BytecodeVM.hpp
//
//===----------------------------------------------------------------------===//
//
// This file defines the compact bytecode format used by the Zanna bytecode VM.
// Bytecode is compiled from IL at module load time and interpreted for fast
// execution compared to direct IL interpretation.
//
// Instruction Encoding:
// - 32-bit fixed-width primary format: [opcode:8][arg0:8][arg1:8][arg2:8]
// - 64-bit extended format for large operands
//
// Stack Model:
// - Stack-based evaluation with local variable slots
// - Parameters mapped to first N locals
// - Operand stack grows upward from locals

/**
 * @file src/bytecode/Bytecode.hpp
 * @brief Defines bytecode opcodes, instruction packing, decoding, and stack
 *        slots.
 *
 * @details
 * Zanna bytecode uses one 32-bit word per primary instruction, with the opcode
 * in the low eight bits and up to 24 argument bits above it. This header
 * centralizes constexpr encoders and decoders so the compiler and interpreter
 * share identical bit layouts.
 *
 * `BCSlot` is an untagged eight-byte union. The active representation is
 * determined solely by verified bytecode and the consuming opcode; the slot
 * itself does not track or own pointer targets.
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace zanna::bytecode {

/// @brief Magic number for bytecode modules: "VBC\x01".
/// @details Stored in little-endian byte order at the beginning of every
///          serialized bytecode module to identify the file format.
constexpr uint32_t kBytecodeModuleMagic = 0x01434256;

/// @brief Current bytecode format version.
/// @details Version 3 adds explicit scalar-width metadata to checked arithmetic,
///          checked division/remainder, bounds checks, and checked f64-to-int
///          conversions so bytecode execution can share IL VM scalar semantics.
constexpr uint32_t kBytecodeVersion = 3;

/// @brief Maximum call stack depth before a StackOverflow trap is raised.
constexpr uint32_t kMaxCallDepth = 4096;

/// @brief Maximum operand stack size (in BCSlot entries) per call frame.
constexpr uint32_t kMaxStackSize = 1024;

/// @brief Bytecode opcodes for the Zanna bytecode VM.
/// @details Opcodes are organized by functional category and assigned to
///          contiguous ranges so that the interpreter's dispatch table
///          benefits from instruction-cache locality.
///
///          Encoding categories:
///          - 0x00-0x0F  Stack operations
///          - 0x10-0x1F  Local variable operations
///          - 0x20-0x2F  Constant loading
///          - 0x30-0x4F  Integer arithmetic
///          - 0x50-0x5F  Float arithmetic
///          - 0x60-0x6F  Bitwise operations
///          - 0x70-0x7F  Integer comparisons
///          - 0x80-0x8F  Float comparisons
///          - 0x90-0x9F  Type conversions
///          - 0xA0-0xAF  Memory operations
///          - 0xB0-0xBF  Control flow
///          - 0xC0-0xCF  Exception handling
///          - 0xD0-0xD7  Debug operations
///          - 0xD8-0xDF  Runtime fast-path operations
///          - 0xE0-0xEF  String operations
enum class BCOpcode : uint8_t {
// The opcode set (names + values) is defined once in bytecode/Bytecode.def and
// expanded here and in opcodeName(); per-opcode descriptions live in that file.
#define BC_OPCODE(name, value) name = value,
#include "bytecode/Bytecode.def"
#undef BC_OPCODE
};

/// @brief Get the human-readable name for an opcode.
/// @details Used for disassembly output, debug logging, and diagnostic messages.
/// @param op The bytecode opcode to look up.
/// @return A NUL-terminated string naming the opcode (e.g., "ADD_I64").
///         Returns "UNKNOWN" for unrecognized opcode values.
const char *opcodeName(BCOpcode op);

/// @brief Check whether an opcode is a basic-block terminator.
/// @details Terminators are instructions that transfer control flow out of the
///          current basic block (jumps, returns, traps, resumes). The compiler
///          and verifier use this to identify block boundaries.
/// @param op The opcode to test.
/// @return True if the opcode ends a basic block; false otherwise.
inline constexpr bool isTerminator(BCOpcode op) {
    return op == BCOpcode::JUMP || op == BCOpcode::JUMP_IF_TRUE || op == BCOpcode::JUMP_IF_FALSE ||
           op == BCOpcode::JUMP_LONG || op == BCOpcode::SWITCH || op == BCOpcode::RETURN ||
           op == BCOpcode::RETURN_VOID || op == BCOpcode::TAIL_CALL || op == BCOpcode::TRAP ||
           op == BCOpcode::TRAP_FROM_ERR || op == BCOpcode::RESUME_SAME ||
           op == BCOpcode::RESUME_NEXT || op == BCOpcode::RESUME_LABEL;
}

/// @brief Check whether an opcode can raise a trap (exception).
/// @details Instructions that may trap include checked arithmetic, checked
///          conversions, memory allocation, function calls, and explicit traps.
///          The compiler uses this to determine which instructions require
///          exception handler coverage.
/// @param op The opcode to test.
/// @return True if the opcode may raise a trap; false otherwise.
inline constexpr bool canTrap(BCOpcode op) {
    switch (op) {
        case BCOpcode::ADD_I64_OVF:
        case BCOpcode::SUB_I64_OVF:
        case BCOpcode::MUL_I64_OVF:
        case BCOpcode::SDIV_I64_CHK:
        case BCOpcode::UDIV_I64_CHK:
        case BCOpcode::SREM_I64_CHK:
        case BCOpcode::UREM_I64_CHK:
        case BCOpcode::IDX_CHK:
        case BCOpcode::F64_TO_I64_CHK:
        case BCOpcode::F64_TO_U64_CHK:
        case BCOpcode::I64_NARROW_CHK:
        case BCOpcode::U64_NARROW_CHK:
        case BCOpcode::ALLOCA:
        case BCOpcode::CALL:
        case BCOpcode::CALL_NATIVE:
        case BCOpcode::CALL_INDIRECT:
        case BCOpcode::TRAP:
        case BCOpcode::TRAP_FROM_ERR:
            return true;
        default:
            return false;
    }
}

//==============================================================================
// Instruction Encoding Helpers
//==============================================================================

/// @brief Encode a 32-bit instruction containing only an opcode (no arguments).
/// @details Produces the encoding [opcode:8][0:24].
/// @param op The opcode to encode.
/// @return The 32-bit encoded instruction word.
inline constexpr uint32_t encodeOp(BCOpcode op) {
    return static_cast<uint32_t>(op);
}

/// @brief Encode a 32-bit instruction with an opcode and one unsigned 8-bit argument.
/// @details Produces the encoding [opcode:8][arg0:8][0:16].
/// @param op   The opcode to encode.
/// @param arg0 The 8-bit unsigned argument (e.g., local slot index).
/// @return The 32-bit encoded instruction word.
inline constexpr uint32_t encodeOp8(BCOpcode op, uint8_t arg0) {
    return static_cast<uint32_t>(op) | (static_cast<uint32_t>(arg0) << 8);
}

/// @brief Encode a 32-bit instruction with an opcode and one signed 8-bit argument.
/// @details Produces the encoding [opcode:8][arg0:8][0:16] where arg0 is
///          reinterpreted as unsigned for bit packing.
/// @param op   The opcode to encode.
/// @param arg0 The signed 8-bit argument (e.g., small immediate).
/// @return The 32-bit encoded instruction word.
inline constexpr uint32_t encodeOpI8(BCOpcode op, int8_t arg0) {
    return static_cast<uint32_t>(op) | (static_cast<uint32_t>(static_cast<uint8_t>(arg0)) << 8);
}

/// @brief Encode a 32-bit instruction with an opcode and two unsigned 8-bit arguments.
/// @details Produces the encoding [opcode:8][arg0:8][arg1:8][0:8].
/// @param op   The opcode to encode.
/// @param arg0 First 8-bit unsigned argument.
/// @param arg1 Second 8-bit unsigned argument.
/// @return The 32-bit encoded instruction word.
inline constexpr uint32_t encodeOp88(BCOpcode op, uint8_t arg0, uint8_t arg1) {
    return static_cast<uint32_t>(op) | (static_cast<uint32_t>(arg0) << 8) |
           (static_cast<uint32_t>(arg1) << 16);
}

/// @brief Encode a 32-bit instruction with an opcode and one unsigned 16-bit argument.
/// @details Produces the encoding [opcode:8][arg0_lo:8][arg0_hi:8][0:8].
///          The 16-bit argument occupies bits 8-23.
/// @param op   The opcode to encode.
/// @param arg0 The 16-bit unsigned argument (e.g., wide local index, pool index).
/// @return The 32-bit encoded instruction word.
inline constexpr uint32_t encodeOp16(BCOpcode op, uint16_t arg0) {
    return static_cast<uint32_t>(op) | (static_cast<uint32_t>(arg0) << 8);
}

/// @brief Encode a 32-bit instruction with an opcode and one signed 16-bit argument.
/// @details Produces the encoding [opcode:8][arg0_lo:8][arg0_hi:8][0:8] where
///          the signed value is reinterpreted as unsigned for bit packing.
/// @param op   The opcode to encode.
/// @param arg0 The signed 16-bit argument (e.g., branch offset).
/// @return The 32-bit encoded instruction word.
inline constexpr uint32_t encodeOpI16(BCOpcode op, int16_t arg0) {
    return static_cast<uint32_t>(op) | (static_cast<uint32_t>(static_cast<uint16_t>(arg0)) << 8);
}

/// @brief Encode a 32-bit instruction with an opcode, an 8-bit arg, and a 16-bit arg.
/// @details Produces the encoding [opcode:8][arg0:8][arg1_lo:8][arg1_hi:8].
/// @param op   The opcode to encode.
/// @param arg0 First 8-bit unsigned argument.
/// @param arg1 Second 16-bit unsigned argument.
/// @return The 32-bit encoded instruction word.
inline constexpr uint32_t encodeOp8_16(BCOpcode op, uint8_t arg0, uint16_t arg1) {
    return static_cast<uint32_t>(op) | (static_cast<uint32_t>(arg0) << 8) |
           (static_cast<uint32_t>(arg1) << 16);
}

/// @brief Encode a 32-bit instruction with an opcode and one unsigned 24-bit argument.
/// @details Produces the encoding [opcode:8][arg0:24]. The argument is masked
///          to 24 bits to prevent overflow into the opcode field.
/// @param op   The opcode to encode.
/// @param arg0 The 24-bit unsigned argument (e.g., long jump offset).
/// @return The 32-bit encoded instruction word.
inline constexpr uint32_t encodeOp24(BCOpcode op, uint32_t arg0) {
    return static_cast<uint32_t>(op) | ((arg0 & 0xFFFFFF) << 8);
}

/// @brief Encode a 32-bit instruction with an opcode and one signed 24-bit argument.
/// @details Produces the encoding [opcode:8][arg0:24]. The signed value is
///          masked to 24 bits; sign extension is performed during decoding.
/// @param op   The opcode to encode.
/// @param arg0 The signed 24-bit argument (e.g., signed long jump offset).
/// @return The 32-bit encoded instruction word.
inline constexpr uint32_t encodeOpI24(BCOpcode op, int32_t arg0) {
    return static_cast<uint32_t>(op) | ((static_cast<uint32_t>(arg0) & 0xFFFFFF) << 8);
}

//==============================================================================
// Instruction Decoding Helpers
//==============================================================================

/// @brief Extract the opcode from a 32-bit instruction word.
/// @param instr The encoded instruction word.
/// @return The BCOpcode stored in bits 0-7.
inline constexpr BCOpcode decodeOpcode(uint32_t instr) {
    return static_cast<BCOpcode>(instr & 0xFF);
}

/// @brief Whether @p byte is a defined bytecode opcode.
/// @details Generated from bytecode/Bytecode.def so it can never drift from the
///          BCOpcode enum. Lets dispatchers validate a raw instruction byte and
///          trap unknown opcodes *before* a (default-less, -Wswitch-exhaustive)
///          dispatch switch — keeping invalid-byte safety without a `default:`.
/// @param byte Raw low-order opcode byte extracted from an instruction word.
/// @return `true` when `byte` appears in `Bytecode.def`; otherwise `false`.
inline constexpr bool isKnownOpcode(uint8_t byte) {
    switch (byte) {
#define BC_OPCODE(name, value) case (value):
#include "bytecode/Bytecode.def"
#undef BC_OPCODE
        return true;
        default:
            return false;
    }
}

/// @brief Extract the first unsigned 8-bit argument (bits 8-15).
/// @param instr The encoded instruction word.
/// @return The unsigned 8-bit value in the arg0 field.
inline constexpr uint8_t decodeArg8_0(uint32_t instr) {
    return static_cast<uint8_t>((instr >> 8) & 0xFF);
}

/// @brief Extract the first signed 8-bit argument (bits 8-15).
/// @param instr The encoded instruction word.
/// @return The signed 8-bit value in the arg0 field.
inline constexpr int8_t decodeArgI8_0(uint32_t instr) {
    return static_cast<int8_t>((instr >> 8) & 0xFF);
}

/// @brief Extract the second unsigned 8-bit argument (bits 16-23).
/// @param instr The encoded instruction word.
/// @return The unsigned 8-bit value in the arg1 field.
inline constexpr uint8_t decodeArg8_1(uint32_t instr) {
    return static_cast<uint8_t>((instr >> 16) & 0xFF);
}

/// @brief Extract the third unsigned 8-bit argument (bits 24-31).
/// @param instr The encoded instruction word.
/// @return The unsigned 8-bit value in the arg2 field.
inline constexpr uint8_t decodeArg8_2(uint32_t instr) {
    return static_cast<uint8_t>((instr >> 24) & 0xFF);
}

/// @brief Extract a 16-bit unsigned argument from bits 8-23.
/// @param instr The encoded instruction word.
/// @return The unsigned 16-bit value spanning arg0 and arg1 fields.
inline constexpr uint16_t decodeArg16(uint32_t instr) {
    return static_cast<uint16_t>((instr >> 8) & 0xFFFF);
}

/// @brief Extract a 16-bit signed argument from bits 8-23.
/// @param instr The encoded instruction word.
/// @return The signed 16-bit value spanning arg0 and arg1 fields.
inline constexpr int16_t decodeArgI16(uint32_t instr) {
    return static_cast<int16_t>((instr >> 8) & 0xFFFF);
}

/// @brief Extract the second 16-bit unsigned argument from bits 16-31.
/// @param instr The encoded instruction word.
/// @return The unsigned 16-bit value in the upper half of the argument space.
inline constexpr uint16_t decodeArg16_1(uint32_t instr) {
    return static_cast<uint16_t>((instr >> 16) & 0xFFFF);
}

/// @brief Extract a 24-bit unsigned argument from bits 8-31.
/// @param instr The encoded instruction word.
/// @return The unsigned 24-bit value occupying the full argument space.
inline constexpr uint32_t decodeArg24(uint32_t instr) {
    return (instr >> 8) & 0xFFFFFF;
}

/// @brief Extract a 24-bit signed argument from bits 8-31 with sign extension.
/// @details If bit 23 (the sign bit within the 24-bit field) is set, the value
///          is sign-extended to a full 32-bit signed integer.
/// @param instr The encoded instruction word.
/// @return The sign-extended 24-bit signed value.
inline constexpr int32_t decodeArgI24(uint32_t instr) {
    uint32_t raw = (instr >> 8) & 0xFFFFFF;
    // Sign extend from 24-bit
    if (raw & 0x800000) {
        return static_cast<int32_t>(raw | 0xFF000000);
    }
    return static_cast<int32_t>(raw);
}

//==============================================================================
// BCSlot - Runtime Value Type
//==============================================================================

/// @brief Tagged union for runtime values on the operand stack and in local variable slots.
/// @details BCSlot is the fundamental value type used by the bytecode VM. Every
///          entry on the operand stack and every local variable slot holds one BCSlot.
///          The union overlays three representations that share 8 bytes of storage:
///          - i64: 64-bit signed integer (also used for booleans and unsigned values)
///          - f64: IEEE-754 double-precision floating-point
///          - ptr: Generic pointer for objects, strings, and memory references
///
///          The correct interpretation is determined by the opcode that produces or
///          consumes the value. There is no runtime type tag; type safety is ensured
///          at compile time by the bytecode compiler.
/// @invariant sizeof(BCSlot) == 8 (enforced by static_assert).
union BCSlot {
    int64_t i64; ///< Integer representation (also booleans, unsigned values).
    double f64;  ///< IEEE-754 double-precision floating-point representation.
    void *ptr;   ///< Pointer representation (objects, strings, memory addresses).

    /// @brief Default constructor; zero-initializes the integer field.
    BCSlot() : i64(0) {}

    /// @brief Construct a BCSlot holding a 64-bit integer value.
    /// @param v The integer value to store.
    explicit BCSlot(int64_t v) : i64(v) {}

    /// @brief Construct a BCSlot holding a double-precision floating-point value.
    /// @param v The double value to store.
    explicit BCSlot(double v) : f64(v) {}

    /// @brief Construct a BCSlot holding a pointer value.
    /// @param v The pointer value to store (may be nullptr).
    explicit BCSlot(void *v) : ptr(v) {}

    /// @brief Create a BCSlot from a 64-bit integer value.
    /// @param v The integer value.
    /// @return A new BCSlot with the i64 field set.
    static BCSlot fromInt(int64_t v) {
        return BCSlot(v);
    }

    /// @brief Create a BCSlot from a double-precision floating-point value.
    /// @param v The double value.
    /// @return A new BCSlot with the f64 field set.
    static BCSlot fromFloat(double v) {
        return BCSlot(v);
    }

    /// @brief Create a BCSlot from a pointer value.
    /// @param v The pointer value (may be nullptr).
    /// @return A new BCSlot with the ptr field set.
    static BCSlot fromPtr(void *v) {
        return BCSlot(v);
    }

    /// @brief Create a BCSlot holding a null pointer.
    /// @return A new BCSlot with ptr set to nullptr.
    static BCSlot null() {
        return BCSlot(static_cast<void *>(nullptr));
    }
};

static_assert(sizeof(BCSlot) == 8, "BCSlot must be 8 bytes");

} // namespace zanna::bytecode
