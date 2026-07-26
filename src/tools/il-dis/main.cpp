//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the `il-dis` utility. The executable reads textual IL, lowers it
// through the bytecode compiler, and emits a decoded bytecode disassembly.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Disassemble Zanna IL after bytecode lowering.

#include "bytecode/Bytecode.hpp"
#include "bytecode/BytecodeCompiler.hpp"
#include "bytecode/BytecodeModule.hpp"
#include "common/Utf8CommandLine.hpp"
#include "il/core/Type.hpp"
#include "support/diag_expected.hpp"
#include "tools/common/module_loader.hpp"
#include "zanna/version.hpp"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace bc = zanna::bytecode;

/// @brief Command-line options for the il-dis disassembler.
struct Options {
    bool showPools = true; ///< Print constant/global/native pool listings.
    bool showRaw = false;  ///< Include the raw 32-bit instruction words.
    std::string inputPath; ///< Path to the input IL file.
};

/// @brief Outcome of parsing the il-dis command line.
enum class ParseResult { Ok, Help, Version, Error };

/// @brief Print il-dis usage text to @p out.
/// @param out Destination stream.
void usage(std::ostream &out) {
    out << "Usage: il-dis [options] <file.il>\n"
        << "\n"
        << "Parse textual IL, lower it to Zanna bytecode, and print decoded bytecode.\n"
        << "\n"
        << "Options:\n"
        << "  --raw       Include raw 32-bit instruction words\n"
        << "  --no-pools  Omit constant/global/native pool listings\n"
        << "  --version   Show IL version\n"
        << "  -h, --help  Show this help\n";
}

/// @brief Parse il-dis arguments into @p options.
/// @details Recognises --raw, --no-pools, --version, and --help/-h, and exactly
///          one positional input file; errors on unknown options or a missing/
///          duplicate input file.
/// @param argc Argument count.
/// @param argv Argument vector.
/// @param options Receives parsed flags and input path.
/// @param err Destination for argument diagnostics.
/// @return Ok, Help, Version, or Error (with a message written to @p err).
ParseResult parseArgs(int argc, char **argv, Options &options, std::ostream &err) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            return ParseResult::Help;
        }
        if (arg == "--version") {
            return ParseResult::Version;
        }
        if (arg == "--raw") {
            options.showRaw = true;
            continue;
        }
        if (arg == "--no-pools") {
            options.showPools = false;
            continue;
        }
        if (!arg.empty() && arg.front() == '-') {
            err << "il-dis: unknown option '" << arg << "'\n";
            return ParseResult::Error;
        }
        if (!options.inputPath.empty()) {
            err << "il-dis: expected one input file, got both '" << options.inputPath << "' and '"
                << arg << "'\n";
            return ParseResult::Error;
        }
        options.inputPath = arg;
    }

    if (options.inputPath.empty()) {
        err << "il-dis: missing input file\n";
        return ParseResult::Error;
    }

    return ParseResult::Ok;
}

/// @brief Format a 32-bit value as "0x" + 8 uppercase hex digits.
/// @param value Word to format.
/// @return Fixed-width hexadecimal text.
std::string hexWord(uint32_t value) {
    std::ostringstream os;
    os << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << value;
    return os.str();
}

/// @brief Format a byte as "0x" + 2 uppercase hex digits.
/// @param value Byte to format.
/// @return Fixed-width hexadecimal text.
std::string hexByte(uint8_t value) {
    std::ostringstream os;
    os << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(2)
       << static_cast<unsigned>(value);
    return os.str();
}

/// @brief Format a program counter as a zero-padded decimal of the given width.
/// @param pc Program-counter word index.
/// @param width Minimum decimal column width.
/// @return Zero-padded decimal text.
std::string pcText(uint32_t pc, unsigned width) {
    std::ostringstream os;
    os << std::setfill('0') << std::setw(static_cast<int>(width)) << pc;
    return os.str();
}

/// @brief Choose the pc column width (digits) needed for a code of @p codeSize.
/// @param codeSize Function length in instruction words.
/// @return At least four decimal digits, expanded for larger functions.
unsigned pcWidth(size_t codeSize) {
    unsigned width = 4;
    for (size_t limit = 10000; codeSize >= limit; limit *= 10)
        ++width;
    return width;
}

/// @brief Render @p text as a quoted, escaped string literal for disassembly output.
/// @param text Raw string-pool value.
/// @return Quoted representation with controls and delimiters escaped.
std::string escapeString(std::string_view text) {
    std::ostringstream os;
    os << '"';
    for (unsigned char ch : text) {
        switch (ch) {
            case '\\':
                os << "\\\\";
                break;
            case '"':
                os << "\\\"";
                break;
            case '\n':
                os << "\\n";
                break;
            case '\r':
                os << "\\r";
                break;
            case '\t':
                os << "\\t";
                break;
            case '\0':
                os << "\\0";
                break;
            default:
                if (ch < 0x20 || ch == 0x7F) {
                    os << "\\x" << std::hex << std::uppercase << std::setfill('0') << std::setw(2)
                       << static_cast<unsigned>(ch) << std::dec;
                } else {
                    os << static_cast<char>(ch);
                }
                break;
        }
    }
    os << '"';
    return os.str();
}

/// @brief Describe a branch target as "pc=<abs> (offset=<rel>)".
/// @details Computes the absolute target from @p basePc + @p offset and flags it
///          "[out-of-range]" when it falls outside [0, codeSize].
/// @param basePc PC relative to which the signed offset is interpreted.
/// @param offset Signed branch displacement.
/// @param codeSize Function code length.
/// @return Human-readable absolute/relative target description.
std::string targetText(int64_t basePc, int32_t offset, size_t codeSize) {
    const int64_t target = basePc + static_cast<int64_t>(offset);
    std::ostringstream os;
    os << "pc=" << target << " (offset=" << offset << ')';
    if (target < 0 || target > static_cast<int64_t>(codeSize)) {
        os << " [out-of-range]";
    }
    return os.str();
}

/// @brief Return the textual name of an IL type.
/// @param type IL type to render.
/// @return Canonical type spelling.
std::string typeName(const il::core::Type &type) {
    return type.toString();
}

/// @brief Map a narrow-integer type tag to its IL type name (i1/i16/i32/i64).
/// @param tag Encoded narrow-integer tag.
/// @return Type name or an explicit unknown-tag marker.
std::string narrowTypeName(uint8_t tag) {
    switch (tag) {
        case 0:
            return "i1";
        case 1:
            return "i16";
        case 2:
            return "i32";
        case 3:
            return "i64";
        default:
            return "type#" + std::to_string(tag);
    }
}

/// @brief Map a trap-kind byte to its human-readable name (e.g. "DivideByZero").
/// @param kind Encoded runtime trap kind.
/// @return Known name or a numeric fallback marker.
std::string trapKindName(uint8_t kind) {
    switch (kind) {
        case 0:
            return "DivideByZero";
        case 1:
            return "Overflow";
        case 2:
            return "InvalidCast";
        case 3:
            return "DomainError";
        case 4:
            return "Bounds";
        case 5:
            return "FileNotFound";
        case 6:
            return "EndOfFile";
        case 7:
            return "IOError";
        case 8:
            return "InvalidOperation";
        case 9:
            return "RuntimeError";
        case 10:
            return "Interrupt";
        case 11:
            return "NetworkError";
        case 100:
            return "NullPointer";
        case 101:
            return "StackOverflow";
        case 102:
            return "InvalidOpcode";
        case 255:
            return "None";
        default:
            return "TrapKind#" + std::to_string(kind);
    }
}

/// @brief Look up an item's @c name by index, or a "<fallback>#N [invalid]" string.
/// @details Used to render pool references safely even when an index is corrupt.
/// @param items Indexed named records.
/// @param index Candidate index.
/// @param fallback Label used for invalid indexes.
/// @return Item name or invalid-index marker.
template <typename T>
std::string indexedName(const std::vector<T> &items, uint32_t index, std::string_view fallback) {
    if (index < items.size()) {
        return items[index].name;
    }
    return std::string(fallback) + "#" + std::to_string(index) + " [invalid]";
}

/// @brief Resolve a function-pool index to its name (or an [invalid] marker).
/// @param module Bytecode module owning the function pool.
/// @param index Candidate function index.
/// @return Function name or invalid-index marker.
std::string functionName(const bc::BytecodeModule &module, uint32_t index) {
    return indexedName(module.functions, index, "function");
}

/// @brief Resolve a native-function-pool index to its name.
/// @param module Bytecode module owning the native pool.
/// @param index Candidate native index.
/// @return Native name or invalid-index marker.
std::string nativeName(const bc::BytecodeModule &module, uint32_t index) {
    return indexedName(module.nativeFuncs, index, "native");
}

/// @brief Resolve a global-pool index to its name.
/// @param module Bytecode module owning the global pool.
/// @param index Candidate global index.
/// @return Global name or invalid-index marker.
std::string globalName(const bc::BytecodeModule &module, uint32_t index) {
    return indexedName(module.globals, index, "global");
}

/// @brief Resolve a 1-based source-file table entry to its path ("" for entry 0).
/// @param module Bytecode module owning the source table.
/// @param tableEntry One-based encoded table entry.
/// @return Source path, empty value, or invalid-index marker.
std::string sourceFileName(const bc::BytecodeModule &module, uint32_t tableEntry) {
    if (tableEntry == 0) {
        return {};
    }
    const uint32_t index = tableEntry - 1;
    if (index < module.sourceFiles.size()) {
        return module.sourceFiles[index].path;
    }
    return "source#" + std::to_string(index) + " [invalid]";
}

/// @brief Append @p text to @p comments when it is non-empty.
/// @param comments Destination comment fragments.
/// @param text Candidate fragment, moved when non-empty.
void appendComment(std::vector<std::string> &comments, std::string text) {
    if (!text.empty()) {
        comments.push_back(std::move(text));
    }
}

/// @brief Join comment fragments with "; " for a single trailing comment column.
/// @param comments Ordered fragments.
/// @return Delimiter-joined comment text.
std::string joinComments(const std::vector<std::string> &comments) {
    std::ostringstream os;
    for (size_t i = 0; i < comments.size(); ++i) {
        if (i != 0) {
            os << "; ";
        }
        os << comments[i];
    }
    return os.str();
}

/// @brief Append a "<source>:line N" debug comment for the instruction at @p pc.
/// @details Reads the function's line and source-file tables; emits nothing when
///          no debug info is present for @p pc.
/// @param module Module owning source paths.
/// @param fn Function owning debug side tables.
/// @param pc Instruction word index.
/// @param comments Destination fragments.
void appendDebugComment(const bc::BytecodeModule &module,
                        const bc::BytecodeFunction &fn,
                        uint32_t pc,
                        std::vector<std::string> &comments) {
    uint32_t line = 0;
    if (pc < fn.lineTable.size()) {
        line = fn.lineTable[pc];
    }
    uint32_t sourceEntry = 0;
    if (pc < fn.sourceFileTable.size()) {
        sourceEntry = fn.sourceFileTable[pc];
    }
    if (line == 0 && sourceEntry == 0) {
        return;
    }

    std::ostringstream os;
    const std::string source = sourceFileName(module, sourceEntry);
    if (!source.empty()) {
        os << source;
        if (line != 0) {
            os << ':';
        }
    }
    if (line != 0) {
        os << "line " << line;
    }
    appendComment(comments, os.str());
}

/// @brief Return true if @p pc indexes a valid instruction word in @p fn.code.
/// @param fn Function whose code is queried.
/// @param pc Candidate word index.
/// @return true when @p pc is in bounds.
bool hasWord(const bc::BytecodeFunction &fn, uint32_t pc) {
    return pc < fn.code.size();
}

/// @brief Decode and print one bytecode instruction at @p pc.
/// @details Decodes the opcode and its operands, formats branch targets and pool
///          references into readable text, appends debug-info and immediate-word
///          comments, and (when @p options.showRaw) the raw 32-bit words. Multi-word
///          instructions consume the trailing words.
/// @param module Owning module (for pool/source lookups).
/// @param fn Function whose code is being disassembled.
/// @param pc Program counter of the instruction to decode.
/// @param width PC column width for alignment.
/// @param options Output options (raw words, etc.).
/// @param out Destination stream.
/// @return The pc of the next instruction.
uint32_t disassembleInstruction(const bc::BytecodeModule &module,
                                const bc::BytecodeFunction &fn,
                                uint32_t pc,
                                unsigned width,
                                const Options &options,
                                std::ostream &out) {
    const uint32_t word = fn.code[pc];
    const auto rawOpcode = static_cast<uint8_t>(word & 0xFFu);
    const bc::BCOpcode op = bc::decodeOpcode(word);
    std::string opcode =
        bc::isKnownOpcode(rawOpcode) ? bc::opcodeName(op) : "UNKNOWN_" + hexByte(rawOpcode);
    std::ostringstream operands;
    std::vector<std::string> comments;
    std::vector<std::string> extraLines;
    uint32_t nextPc = pc + 1;

    switch (op) {
        case bc::BCOpcode::LOAD_LOCAL:
        case bc::BCOpcode::STORE_LOCAL:
        case bc::BCOpcode::INC_LOCAL:
        case bc::BCOpcode::DEC_LOCAL:
            operands << "local=" << static_cast<unsigned>(bc::decodeArg8_0(word));
            break;

        case bc::BCOpcode::LOAD_LOCAL_W:
        case bc::BCOpcode::STORE_LOCAL_W:
            operands << "local=" << bc::decodeArg16(word);
            break;

        case bc::BCOpcode::LOAD_I8:
            operands << static_cast<int>(bc::decodeArgI8_0(word));
            break;

        case bc::BCOpcode::LOAD_I16:
            operands << bc::decodeArgI16(word);
            break;

        case bc::BCOpcode::LOAD_I32:
            if (hasWord(fn, pc + 1)) {
                const auto value = static_cast<int32_t>(fn.code[pc + 1]);
                operands << value;
                appendComment(comments, "imm32_word=" + hexWord(fn.code[pc + 1]));
                nextPc = pc + 2;
            } else {
                operands << "<missing-i32-word>";
                appendComment(comments, "malformed extended immediate");
            }
            break;

        case bc::BCOpcode::LOAD_I64: {
            const uint16_t index = bc::decodeArg16(word);
            operands << "i64_pool[" << index << ']';
            if (index < module.i64Pool.size()) {
                appendComment(comments, std::to_string(module.i64Pool[index]));
            } else {
                appendComment(comments, "invalid i64 pool index");
            }
            break;
        }

        case bc::BCOpcode::LOAD_F64: {
            const uint16_t index = bc::decodeArg16(word);
            operands << "f64_pool[" << index << ']';
            if (index < module.f64Pool.size()) {
                std::ostringstream value;
                value << module.f64Pool[index];
                appendComment(comments, value.str());
            } else {
                appendComment(comments, "invalid f64 pool index");
            }
            break;
        }

        case bc::BCOpcode::LOAD_STR: {
            const uint16_t index = bc::decodeArg16(word);
            operands << "string_pool[" << index << ']';
            if (index < module.stringPool.size()) {
                appendComment(comments, escapeString(module.stringPool[index]));
            } else {
                appendComment(comments, "invalid string pool index");
            }
            break;
        }

        case bc::BCOpcode::LOAD_GLOBAL:
        case bc::BCOpcode::STORE_GLOBAL:
        case bc::BCOpcode::LOAD_GLOBAL_ADDR: {
            const uint16_t index = bc::decodeArg16(word);
            operands << "global[" << index << ']';
            appendComment(comments, globalName(module, index));
            break;
        }

        case bc::BCOpcode::ADD_I64_OVF:
        case bc::BCOpcode::SUB_I64_OVF:
        case bc::BCOpcode::MUL_I64_OVF:
        case bc::BCOpcode::I64_NARROW_CHK:
        case bc::BCOpcode::U64_NARROW_CHK: {
            const uint8_t tag = bc::decodeArg8_0(word);
            operands << "target=" << narrowTypeName(tag);
            break;
        }

        case bc::BCOpcode::JUMP:
        case bc::BCOpcode::JUMP_IF_TRUE:
        case bc::BCOpcode::JUMP_IF_FALSE: {
            const int16_t offset = bc::decodeArgI16(word);
            operands << targetText(static_cast<int64_t>(pc) + 1, offset, fn.code.size());
            break;
        }

        case bc::BCOpcode::JUMP_LONG: {
            const int32_t offset = bc::decodeArgI24(word);
            operands << targetText(static_cast<int64_t>(pc) + 1, offset, fn.code.size());
            break;
        }

        case bc::BCOpcode::SWITCH: {
            if (!hasWord(fn, pc + 1) || !hasWord(fn, pc + 2)) {
                operands << "<truncated-switch>";
                appendComment(comments, "missing switch header");
                break;
            }

            const uint32_t numCases = fn.code[pc + 1];
            const auto defaultOffset = static_cast<int32_t>(fn.code[pc + 2]);
            operands << "cases=" << numCases;
            extraLines.push_back("default -> " + targetText(static_cast<int64_t>(pc + 2),
                                                            defaultOffset,
                                                            fn.code.size()));

            const uint64_t tableWords = static_cast<uint64_t>(numCases) * 2u;
            const uint64_t tableStart = static_cast<uint64_t>(pc) + 3u;
            const uint64_t next = tableStart + tableWords;
            if (tableWords > std::numeric_limits<uint32_t>::max() ||
                next > static_cast<uint64_t>(fn.code.size())) {
                appendComment(comments, "truncated switch table");
                nextPc = static_cast<uint32_t>(fn.code.size());
                break;
            }

            for (uint32_t i = 0; i < numCases; ++i) {
                const uint32_t caseValuePc = pc + 3 + (i * 2);
                const uint32_t caseOffsetPc = caseValuePc + 1;
                const auto caseValue = static_cast<int32_t>(fn.code[caseValuePc]);
                const auto caseOffset = static_cast<int32_t>(fn.code[caseOffsetPc]);
                extraLines.push_back(
                    "case " + std::to_string(caseValue) + " -> " +
                    targetText(static_cast<int64_t>(caseOffsetPc), caseOffset, fn.code.size()));
            }
            nextPc = static_cast<uint32_t>(next);
            break;
        }

        case bc::BCOpcode::CALL: {
            const uint16_t index = bc::decodeArg16(word);
            operands << "function[" << index << ']';
            appendComment(comments, functionName(module, index));
            break;
        }

        case bc::BCOpcode::CALL_NATIVE: {
            const uint8_t argCount = bc::decodeArg8_0(word);
            const uint16_t nativeIndex = bc::decodeArg16_1(word);
            operands << "args=" << static_cast<unsigned>(argCount) << " native[" << nativeIndex
                     << ']';
            appendComment(comments, nativeName(module, nativeIndex));
            break;
        }

        case bc::BCOpcode::CALL_INDIRECT:
            operands << "args=" << static_cast<unsigned>(bc::decodeArg8_0(word));
            break;

        case bc::BCOpcode::EH_PUSH:
            if (hasWord(fn, pc + 1)) {
                const auto offset = static_cast<int32_t>(fn.code[pc + 1]);
                operands << targetText(static_cast<int64_t>(pc + 1), offset, fn.code.size());
                appendComment(comments, "offset_word=" + hexWord(fn.code[pc + 1]));
                nextPc = pc + 2;
            } else {
                operands << "<missing-handler-offset>";
                appendComment(comments, "malformed handler offset");
            }
            break;

        case bc::BCOpcode::TRAP: {
            const uint8_t kind = bc::decodeArg8_0(word);
            operands << static_cast<unsigned>(kind);
            appendComment(comments, trapKindName(kind));
            break;
        }

        case bc::BCOpcode::RESUME_LABEL:
            if (hasWord(fn, pc + 1)) {
                const auto offset = static_cast<int32_t>(fn.code[pc + 1]);
                operands << targetText(static_cast<int64_t>(pc + 1), offset, fn.code.size());
                appendComment(comments, "offset_word=" + hexWord(fn.code[pc + 1]));
                nextPc = pc + 2;
            } else {
                operands << "<missing-resume-offset>";
                appendComment(comments, "malformed resume offset");
            }
            break;

        case bc::BCOpcode::NOP:
        case bc::BCOpcode::DUP:
        case bc::BCOpcode::DUP2:
        case bc::BCOpcode::POP:
        case bc::BCOpcode::POP2:
        case bc::BCOpcode::SWAP:
        case bc::BCOpcode::ROT3:
        case bc::BCOpcode::LOAD_NULL:
        case bc::BCOpcode::LOAD_ZERO:
        case bc::BCOpcode::LOAD_ONE:
        case bc::BCOpcode::ADD_I64:
        case bc::BCOpcode::SUB_I64:
        case bc::BCOpcode::MUL_I64:
        case bc::BCOpcode::SDIV_I64:
        case bc::BCOpcode::UDIV_I64:
        case bc::BCOpcode::SREM_I64:
        case bc::BCOpcode::UREM_I64:
        case bc::BCOpcode::NEG_I64:
        case bc::BCOpcode::SDIV_I64_CHK:
        case bc::BCOpcode::UDIV_I64_CHK:
        case bc::BCOpcode::SREM_I64_CHK:
        case bc::BCOpcode::UREM_I64_CHK:
        case bc::BCOpcode::IDX_CHK:
        case bc::BCOpcode::ADD_F64:
        case bc::BCOpcode::SUB_F64:
        case bc::BCOpcode::MUL_F64:
        case bc::BCOpcode::DIV_F64:
        case bc::BCOpcode::NEG_F64:
        case bc::BCOpcode::AND_I64:
        case bc::BCOpcode::OR_I64:
        case bc::BCOpcode::XOR_I64:
        case bc::BCOpcode::NOT_I64:
        case bc::BCOpcode::SHL_I64:
        case bc::BCOpcode::LSHR_I64:
        case bc::BCOpcode::ASHR_I64:
        case bc::BCOpcode::CMP_EQ_I64:
        case bc::BCOpcode::CMP_NE_I64:
        case bc::BCOpcode::CMP_SLT_I64:
        case bc::BCOpcode::CMP_SLE_I64:
        case bc::BCOpcode::CMP_SGT_I64:
        case bc::BCOpcode::CMP_SGE_I64:
        case bc::BCOpcode::CMP_ULT_I64:
        case bc::BCOpcode::CMP_ULE_I64:
        case bc::BCOpcode::CMP_UGT_I64:
        case bc::BCOpcode::CMP_UGE_I64:
        case bc::BCOpcode::CMP_EQ_F64:
        case bc::BCOpcode::CMP_NE_F64:
        case bc::BCOpcode::CMP_LT_F64:
        case bc::BCOpcode::CMP_LE_F64:
        case bc::BCOpcode::CMP_GT_F64:
        case bc::BCOpcode::CMP_GE_F64:
        case bc::BCOpcode::CMP_ORD_F64:
        case bc::BCOpcode::CMP_UNO_F64:
        case bc::BCOpcode::I64_TO_F64:
        case bc::BCOpcode::U64_TO_F64:
        case bc::BCOpcode::F64_TO_I64:
        case bc::BCOpcode::F64_TO_I64_CHK:
        case bc::BCOpcode::F64_TO_U64_CHK:
        case bc::BCOpcode::BOOL_TO_I64:
        case bc::BCOpcode::I64_TO_BOOL:
        case bc::BCOpcode::ALLOCA:
        case bc::BCOpcode::GEP:
        case bc::BCOpcode::LOAD_I8_MEM:
        case bc::BCOpcode::LOAD_I16_MEM:
        case bc::BCOpcode::LOAD_I32_MEM:
        case bc::BCOpcode::LOAD_I64_MEM:
        case bc::BCOpcode::LOAD_F64_MEM:
        case bc::BCOpcode::LOAD_PTR_MEM:
        case bc::BCOpcode::LOAD_STR_MEM:
        case bc::BCOpcode::STORE_I8_MEM:
        case bc::BCOpcode::STORE_I16_MEM:
        case bc::BCOpcode::STORE_I32_MEM:
        case bc::BCOpcode::STORE_I64_MEM:
        case bc::BCOpcode::STORE_F64_MEM:
        case bc::BCOpcode::STORE_PTR_MEM:
        case bc::BCOpcode::STORE_STR_MEM:
        case bc::BCOpcode::RETURN:
        case bc::BCOpcode::RETURN_VOID:
        case bc::BCOpcode::TAIL_CALL:
        case bc::BCOpcode::EH_POP:
        case bc::BCOpcode::EH_ENTRY:
        case bc::BCOpcode::TRAP_FROM_ERR:
        case bc::BCOpcode::MAKE_ERROR:
        case bc::BCOpcode::ERR_GET_KIND:
        case bc::BCOpcode::ERR_GET_CODE:
        case bc::BCOpcode::ERR_GET_IP:
        case bc::BCOpcode::ERR_GET_LINE:
        case bc::BCOpcode::ERR_GET_MSG:
        case bc::BCOpcode::RESUME_SAME:
        case bc::BCOpcode::RESUME_NEXT:
        case bc::BCOpcode::TRAP_KIND:
        case bc::BCOpcode::LINE:
        case bc::BCOpcode::BREAKPOINT:
        case bc::BCOpcode::WATCH_VAR:
        case bc::BCOpcode::ARR_I32_GET_FAST:
        case bc::BCOpcode::ARR_I32_SET_FAST:
        case bc::BCOpcode::ARR_I64_GET_FAST:
        case bc::BCOpcode::ARR_I64_SET_FAST:
        case bc::BCOpcode::ARR_F64_GET_FAST:
        case bc::BCOpcode::ARR_F64_SET_FAST:
        case bc::BCOpcode::STR_RETAIN:
        case bc::BCOpcode::STR_RELEASE:
        case bc::BCOpcode::SELECT:
        case bc::BCOpcode::OPCODE_COUNT:
            break;
    }

    appendDebugComment(module, fn, pc, comments);

    out << "    " << pcText(pc, width) << ": ";
    if (options.showRaw) {
        out << hexWord(word) << ' ';
    }
    out << std::left << std::setw(18) << opcode << std::right;
    const std::string operandText = operands.str();
    if (!operandText.empty()) {
        out << ' ' << operandText;
    }
    if (!comments.empty()) {
        out << " ; " << joinComments(comments);
    }
    out << '\n';

    const std::string extraIndent(width + (options.showRaw ? 19 : 8), ' ');
    for (const auto &line : extraLines) {
        out << extraIndent << line << '\n';
    }

    return nextPc;
}

/// @brief Print the module's constant/global/native/function pools to @p out.
/// @param module Module whose pools are rendered.
/// @param out Destination stream.
void printPools(const bc::BytecodeModule &module, std::ostream &out) {
    out << "pools:\n";
    out << "  i64_pool count=" << module.i64Pool.size() << '\n';
    for (size_t i = 0; i < module.i64Pool.size(); ++i) {
        out << "    [" << i << "] " << module.i64Pool[i] << '\n';
    }

    out << "  f64_pool count=" << module.f64Pool.size() << '\n';
    for (size_t i = 0; i < module.f64Pool.size(); ++i) {
        out << "    [" << i << "] " << module.f64Pool[i] << '\n';
    }

    out << "  string_pool count=" << module.stringPool.size() << '\n';
    for (size_t i = 0; i < module.stringPool.size(); ++i) {
        out << "    [" << i << "] " << escapeString(module.stringPool[i]) << '\n';
    }

    out << "  globals count=" << module.globals.size() << '\n';
    for (size_t i = 0; i < module.globals.size(); ++i) {
        const auto &global = module.globals[i];
        out << "    [" << i << "] @" << global.name << " type=" << typeName(global.type)
            << " size=" << global.size << " align=" << global.align;
        if (!global.initString.empty()) {
            out << " init_str=" << escapeString(global.initString);
        } else if (!global.initData.empty()) {
            out << " init_bytes=" << global.initData.size();
        }
        out << '\n';
    }

    out << "  natives count=" << module.nativeFuncs.size() << '\n';
    for (size_t i = 0; i < module.nativeFuncs.size(); ++i) {
        const auto &native = module.nativeFuncs[i];
        out << "    [" << i << "] @" << native.name << " params=" << native.paramCount
            << " returns=" << (native.hasReturn ? "yes" : "no");
        if (native.returnsString) {
            out << " returns_str";
        }
        if (native.consumesClonedStringArgs) {
            out << " consumes_cloned_str_args";
        }
        if (native.consumesOwnedStringArgs) {
            out << " consumes_owned_str_args";
        }
        out << '\n';
    }

    out << "  sources count=" << module.sourceFiles.size() << '\n';
    for (size_t i = 0; i < module.sourceFiles.size(); ++i) {
        out << "    [" << i << "] " << module.sourceFiles[i].path;
        if (module.sourceFiles[i].checksum != 0) {
            out << " checksum=" << module.sourceFiles[i].checksum;
        }
        out << '\n';
    }
}

/// @brief Print a function's header line (params, locals, stack, source, etc.).
/// @param module Owning module used for source lookup.
/// @param fn Function metadata to render.
/// @param index Module function index.
/// @param out Destination stream.
void printFunctionMetadata(const bc::BytecodeModule &module,
                           const bc::BytecodeFunction &fn,
                           uint32_t index,
                           std::ostream &out) {
    out << "function[" << index << "] @" << fn.name << " params=" << fn.numParams
        << " locals=" << fn.numLocals << " max_stack=" << fn.maxStack << " alloca=" << fn.allocaSize
        << " returns=" << (fn.hasReturn ? "yes" : "no") << " words=" << fn.code.size();
    if (fn.sourceFileIdx < module.sourceFiles.size()) {
        out << " source=" << module.sourceFiles[fn.sourceFileIdx].path;
    }
    out << '\n';
}

/// @brief Print a function's side tables: exception ranges, switch tables, and
///        local-variable debug info (each section omitted when empty).
/// @param fn Function whose side tables are rendered.
/// @param out Destination stream.
void printFunctionSideTables(const bc::BytecodeFunction &fn, std::ostream &out) {
    if (!fn.exceptionRanges.empty()) {
        out << "  exception_ranges:\n";
        for (const auto &range : fn.exceptionRanges) {
            out << "    [" << range.startPc << ", " << range.endPc
                << ") handler=pc=" << range.handlerPc << '\n';
        }
    }

    if (!fn.switchTables.empty()) {
        out << "  switch_tables:\n";
        for (size_t i = 0; i < fn.switchTables.size(); ++i) {
            const auto &table = fn.switchTables[i];
            out << "    [" << i << "] default=pc=" << table.defaultPc
                << " cases=" << table.entries.size() << '\n';
            for (const auto &entry : table.entries) {
                out << "      case " << entry.value << " -> pc=" << entry.targetPc << '\n';
            }
        }
    }

    if (!fn.localVars.empty()) {
        out << "  locals_debug:\n";
        for (const auto &local : fn.localVars) {
            out << "    local=" << local.localIdx << " " << local.name << " live=[" << local.startPc
                << ", " << local.endPc << ")\n";
        }
    }
}

/// @brief Disassemble an entire bytecode module to @p out.
/// @details Prints the module header, optional pools, then each function's
///          metadata, instructions (via disassembleInstruction), and side tables.
/// @param module Compiled bytecode module.
/// @param options Output-selection flags.
/// @param out Destination stream.
void disassemble(const bc::BytecodeModule &module, const Options &options, std::ostream &out) {
    out << "bytecode_module magic=" << hexWord(module.magic) << " version=" << module.version
        << " flags=" << module.flags << " functions=" << module.functions.size() << '\n';

    if (options.showPools) {
        printPools(module, out);
    }

    out << "functions:\n";
    for (size_t i = 0; i < module.functions.size(); ++i) {
        const auto &fn = module.functions[i];
        printFunctionMetadata(module, fn, static_cast<uint32_t>(i), out);
        printFunctionSideTables(fn, out);

        const unsigned width = pcWidth(fn.code.size());
        std::string lastBlock;
        for (uint32_t pc = 0; pc < fn.code.size();) {
            std::string block;
            if (pc < fn.blockLabelTable.size()) {
                block = fn.blockLabelTable[pc];
            }
            if (!block.empty() && block != lastBlock) {
                out << "  ^" << block << ":\n";
                lastBlock = block;
            }
            const uint32_t nextPc = disassembleInstruction(module, fn, pc, width, options, out);
            if (nextPc <= pc) {
                out << "    " << pcText(pc, width)
                    << ": <decoder did not advance; stopping function>\n";
                break;
            }
            pc = nextPc;
        }
        out << '\n';
    }
}

} // namespace

/// @brief Entry point for the `il-dis` disassembler.
/// @details Parses arguments, loads and verifies the IL module, lowers it to
///          bytecode via BytecodeCompiler, and prints the decoded disassembly.
/// @param argc Argument count from the C runtime.
/// @param argv Argument vector from the C runtime.
/// @return 0 on success; non-zero on argument, load, or compile errors.
int main(int argc, char **argv) {
    zanna::tools::Utf8CommandLine commandLine(argc, argv);
    if (!commandLine.applyOrReport(argc, argv))
        return 1;
    Options options;
    const ParseResult args = parseArgs(argc, argv, options, std::cerr);
    switch (args) {
        case ParseResult::Help:
            usage(std::cerr);
            return 0;
        case ParseResult::Version:
            std::cout << "IL v" << ZANNA_IL_VERSION_STR << "\n";
            return 0;
        case ParseResult::Error:
            usage(std::cerr);
            return 1;
        case ParseResult::Ok:
            break;
    }

    il::core::Module module;
    auto loaded =
        il::tools::common::loadModuleFromFile(options.inputPath, module, std::cerr, "cannot open ");
    if (!loaded.succeeded()) {
        return 1;
    }

    bc::BytecodeCompiler compiler;
    auto compiled = compiler.compileChecked(module);
    if (!compiled) {
        il::support::printDiag(compiled.error(), std::cerr);
        return 1;
    }

    disassemble(compiled.value(), options, std::cout);
    return 0;
}
