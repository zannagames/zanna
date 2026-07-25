//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/passes/LoweringPass.cpp
// Purpose: Implement the IL lowering pass that adapts front-end IL into the
//          backend's intermediate representation.
// Key invariants:
//   - Value kinds are inferred deterministically and stored alongside SSA IDs.
// Ownership/Lifetime:
//   - Pass mutates the supplied Module in place without owning external state.
// Links: src/codegen/x86_64/passes/LoweringPass.hpp,
//        src/codegen/x86_64/LowerILToMIR.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/x86_64/passes/LoweringPass.hpp"

#include "codegen/common/Parallelism.hpp"
#include "codegen/x86_64/Unsupported.hpp"
#include "common/IntegerHelpers.hpp"
#include "il/core/Global.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/Type.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

/// @file
/// @brief Implements parallel canonical-IL to x86-64 adapter-IL conversion.

namespace zanna::codegen::x64::passes {
namespace {

/// @brief Emit a backend-unsupported diagnostic and terminate lowering.
/// @param detail Specific unsupported or malformed form to report.
/// @throws std::runtime_error Always, through @ref phaseAUnsupported.
[[noreturn]] void reportUnsupported(std::string detail) {
    zanna::codegen::x64::phaseAUnsupported(detail.c_str());
}

/// @brief Adapter module builder that converts IL to backend IR.
/// @details Encapsulates the conversion logic, maintaining state for value kinds
///          and providing helper methods for different instruction categories.
class ModuleAdapter {
  public:
    /// @brief Constructs an adapter with no active module, function, or block.
    explicit ModuleAdapter() = default;

    /// @brief Convert an IL module to the backend adapter representation.
    /// @details Functions are independent at this stage, so a bounded worker
    ///          pool gives each function a private adapter and writes results by
    ///          stable index. Exceptions are captured and rethrown on the caller
    ///          thread to preserve the existing diagnostic behavior.
    /// @param module Canonical IL module to adapt without mutation.
    /// @return Deterministically ordered x86-64 adapter module.
    /// @throws std::exception Re-throws the first worker failure on the caller thread.
    ILModule adapt(const il::core::Module &module) {
        ILModule result{};
        result.funcs.resize(module.functions.size());

        std::atomic_size_t nextIndex{0};
        std::exception_ptr firstException;
        std::mutex exceptionMutex;
        /// @brief Claims function indices and adapts them until work is exhausted.
        /// @details Each invocation owns a private @ref ModuleAdapter for function
        ///          state. The first exception is captured under a mutex for
        ///          deterministic propagation after every worker joins.
        auto adaptNext = [&]() {
            for (;;) {
                const std::size_t index = nextIndex.fetch_add(1, std::memory_order_relaxed);
                if (index >= module.functions.size())
                    return;
                try {
                    ModuleAdapter worker;
                    worker.currentModule_ = &module;
                    result.funcs[index] = worker.adaptFunction(module.functions[index]);
                } catch (...) {
                    std::lock_guard<std::mutex> lock(exceptionMutex);
                    if (!firstException)
                        firstException = std::current_exception();
                    return;
                }
            }
        };

        const std::size_t workerCount =
            common::codegenWorkerCount(module.functions.size());
        if (workerCount <= 1) {
            adaptNext();
        } else {
            std::vector<std::thread> workers;
            workers.reserve(workerCount);
            for (std::size_t worker = 0; worker < workerCount; ++worker)
                workers.emplace_back(adaptNext);
            for (auto &worker : workers)
                worker.join();
        }

        if (firstException)
            std::rethrow_exception(firstException);
        return result;
    }

  private:
    /// @brief Map from SSA ids to their value kinds.
    std::unordered_map<unsigned, ILValue::Kind> valueKinds_{};

    /// @brief Map from SSA ids to their source IL integer widths.
    std::unordered_map<unsigned, std::uint8_t> valueBits_{};

    /// @brief Current function being adapted (for return type access).
    const il::core::Function *currentFunc_{nullptr};

    /// @brief Current basic block being adapted (for diagnostics).
    const il::core::BasicBlock *currentBlock_{nullptr};

    /// @brief Current module being adapted (for global lookup).
    const il::core::Module *currentModule_{nullptr};

    /// @brief Look up a global by name.
    /// @param name Exact module-scope global identifier.
    /// @return Pointer into the current module, or @c nullptr when no module is
    ///         active or no declaration matches.
    const il::core::Global *findGlobal(const std::string &name) const {
        if (!currentModule_)
            return nullptr;
        for (const auto &g : currentModule_->globals) {
            if (g.name == name)
                return &g;
        }
        return nullptr;
    }

    //-------------------------------------------------------------------------
    // Type Conversion
    //-------------------------------------------------------------------------

    /// @brief Map an IL type to the backend adapter value classification.
    /// @param type Canonical IL type to classify.
    /// @return Adapter register/value kind used by x86-64 lowering.
    /// @throws std::runtime_error Through @ref reportUnsupported for void,
    ///         resume-token, or unknown kinds.
    static ILValue::Kind typeToKind(const il::core::Type &type) {
        using il::core::Type;
        switch (type.kind) {
            case Type::Kind::I1:
                return ILValue::Kind::I1;
            case Type::Kind::I16:
            case Type::Kind::I32:
            case Type::Kind::I64:
                return ILValue::Kind::I64;
            case Type::Kind::F64:
                return ILValue::Kind::F64;
            case Type::Kind::Ptr:
                return ILValue::Kind::PTR;
            case Type::Kind::Str:
                return ILValue::Kind::STR;
            case Type::Kind::Error:
                return ILValue::Kind::PTR;
            case Type::Kind::Void:
                reportUnsupported("void-typed value requested by backend adapter");
            case Type::Kind::ResumeTok:
                reportUnsupported("non-scalar IL type encountered during Phase A lowering");
        }
        reportUnsupported("unknown IL type kind encountered during Phase A lowering");
    }

    /// @brief Reconstruct an IL integer type from the adapter's stored bit width.
    /// @param bits Stored source width.
    /// @return @c i16 for 16, @c i32 for 32, and @c i64 for every other value.
    static il::core::Type integerTypeFromBits(std::uint8_t bits) noexcept {
        using Kind = il::core::Type::Kind;
        switch (bits) {
            case 16:
                return il::core::Type(Kind::I16);
            case 32:
                return il::core::Type(Kind::I32);
            default:
                return il::core::Type(Kind::I64);
        }
    }

    /// @brief Return true for opcodes whose omitted textual type has to be inferred.
    /// @param op Canonical IL opcode to inspect.
    /// @return @c true for checked integer arithmetic/division, remainder, and
    ///         index-check opcodes; otherwise @c false.
    static bool hasImplicitIntegerResultWhenVoid(il::core::Opcode op) noexcept {
        using il::core::Opcode;
        switch (op) {
            case Opcode::IAddOvf:
            case Opcode::ISubOvf:
            case Opcode::IMulOvf:
            case Opcode::SDivChk0:
            case Opcode::UDivChk0:
            case Opcode::SRemChk0:
            case Opcode::URemChk0:
            case Opcode::IdxChk:
                return true;
            default:
                return false;
        }
    }

    /// @brief Return whether @p value fits in a signed integer of @p bits.
    /// @param value Signed constant to test.
    /// @param bits Candidate width: 16, 32, or 64.
    /// @return @c true when representable; unsupported widths return @c false.
    static bool constantFitsInIntegerBits(std::int64_t value, std::uint8_t bits) noexcept {
        switch (bits) {
            case 16:
                return value >= std::numeric_limits<std::int16_t>::min() &&
                       value <= std::numeric_limits<std::int16_t>::max();
            case 32:
                return value >= std::numeric_limits<std::int32_t>::min() &&
                       value <= std::numeric_limits<std::int32_t>::max();
            case 64:
                return true;
            default:
                return false;
        }
    }

    /// @brief Pick the verifier's default integer width for an untyped constant.
    /// @param value Signed constant to classify.
    /// @return Smallest of 16, 32, or 64 bits that represents @p value.
    static std::uint8_t smallestIntegerBitsForConstant(std::int64_t value) noexcept {
        if (constantFitsInIntegerBits(value, 16))
            return 16;
        if (constantFitsInIntegerBits(value, 32))
            return 32;
        return 64;
    }

    /// @brief Infer the verifier's result type for an unannotated checked arithmetic op.
    /// @details Uses a common valid width from all temporary operands. Missing
    ///          metadata, non-integer temporaries, mixed widths, or no usable
    ///          operand evidence fall back to @c i64.
    /// @param instr Checked arithmetic or division instruction to inspect.
    /// @return Inferred @c i16, @c i32, or @c i64 type.
    il::core::Type inferCheckedIntegerResultType(const il::core::Instr &instr) const {
        using Kind = il::core::Type::Kind;

        std::optional<std::uint8_t> inferredBits;
        for (const auto &operand : instr.operands) {
            if (operand.kind != il::core::Value::Kind::Temp)
                continue;

            const auto kindIt = valueKinds_.find(operand.id);
            const auto bitsIt = valueBits_.find(operand.id);
            if (kindIt == valueKinds_.end() || bitsIt == valueBits_.end() ||
                kindIt->second != ILValue::Kind::I64) {
                return il::core::Type(Kind::I64);
            }

            const std::uint8_t bits = bitsIt->second;
            if (bits != 16 && bits != 32 && bits != 64) {
                return il::core::Type(Kind::I64);
            }
            if (inferredBits && *inferredBits != bits) {
                return il::core::Type(Kind::I64);
            }
            inferredBits = bits;
        }

        return integerTypeFromBits(inferredBits.value_or(64));
    }

    /// @brief Infer the verifier's result type for an unannotated idx.chk.
    /// @details Reconciles temporary widths with the smallest representable
    ///          widths of integer constants. Any incompatible or unsupported
    ///          operand shape falls back to @c i64.
    /// @param instr Index-check instruction to inspect.
    /// @return Inferred @c i16, @c i32, or @c i64 type.
    il::core::Type inferIdxChkResultType(const il::core::Instr &instr) const {
        using Kind = il::core::Type::Kind;

        std::optional<std::uint8_t> inferredBits;
        for (const auto &operand : instr.operands) {
            std::uint8_t operandBits = 64;
            if (operand.kind == il::core::Value::Kind::Temp) {
                const auto kindIt = valueKinds_.find(operand.id);
                const auto bitsIt = valueBits_.find(operand.id);
                if (kindIt == valueKinds_.end() || bitsIt == valueBits_.end() ||
                    kindIt->second != ILValue::Kind::I64) {
                    return il::core::Type(Kind::I64);
                }

                operandBits = bitsIt->second;
                if (operandBits != 16 && operandBits != 32 && operandBits != 64)
                    return il::core::Type(Kind::I64);
            } else if (operand.kind == il::core::Value::Kind::ConstInt) {
                if (inferredBits) {
                    if (!constantFitsInIntegerBits(operand.i64, *inferredBits))
                        return il::core::Type(Kind::I64);
                    continue;
                }
                operandBits = smallestIntegerBitsForConstant(operand.i64);
            } else {
                return il::core::Type(Kind::I64);
            }

            if (inferredBits && *inferredBits != operandBits)
                return il::core::Type(Kind::I64);
            inferredBits = operandBits;
        }

        return integerTypeFromBits(inferredBits.value_or(64));
    }

    /// @brief Return the type x86-64 lowering should use for @p instr.
    /// @param instr Canonical instruction, possibly carrying a void placeholder type.
    /// @return Explicit source type, an inferred integer type for selected
    ///         unannotated opcodes, or the unchanged void type.
    il::core::Type effectiveInstructionType(const il::core::Instr &instr) const {
        if (instr.type.kind != il::core::Type::Kind::Void)
            return instr.type;
        if (!hasImplicitIntegerResultWhenVoid(instr.op))
            return instr.type;
        if (instr.op == il::core::Opcode::IdxChk)
            return inferIdxChkResultType(instr);
        return inferCheckedIntegerResultType(instr);
    }

    /// @brief Map an IL type to its integer bit width.
    /// @details Used by the backend to populate @c ILInstr::resultBits and
    ///          @c ILValue::bits. All scalar pointer-class kinds report 64
    ///          because they share GPR storage. Void / resume tokens are
    ///          considered programmer errors and trap via @ref reportUnsupported.
    /// @param type Source IL type.
    /// @return Bit width of the type (1, 16, 32, or 64).
    /// @throws std::runtime_error Through @ref reportUnsupported for void,
    ///         resume-token, or unknown kinds.
    static std::uint8_t typeBitWidth(const il::core::Type &type) {
        using il::core::Type;
        switch (type.kind) {
            case Type::Kind::I1:
                return 1;
            case Type::Kind::I16:
                return 16;
            case Type::Kind::I32:
                return 32;
            case Type::Kind::I64:
            case Type::Kind::Ptr:
            case Type::Kind::Str:
            case Type::Kind::Error:
                return 64;
            case Type::Kind::F64:
                return 64;
            case Type::Kind::Void:
                reportUnsupported("void-typed value requested by backend adapter");
            case Type::Kind::ResumeTok:
                reportUnsupported("non-scalar IL type encountered during Phase A lowering");
        }
        reportUnsupported("unknown IL type kind encountered during Phase A lowering");
    }

    //-------------------------------------------------------------------------
    // Value Construction Helpers
    //-------------------------------------------------------------------------

    /// @brief Construct an adapter value representing a block label.
    /// @param name Label spelling transferred into the result.
    /// @return Non-SSA label value with sentinel id -1.
    static ILValue makeLabelValue(std::string name) {
        ILValue label{};
        label.kind = ILValue::Kind::LABEL;
        label.label = std::move(name);
        label.id = -1;
        return label;
    }

    /// @brief Construct an adapter value representing a local block label.
    /// @details Prefixes the label with ".L" to make it assembly-local, avoiding
    ///          symbol collisions between functions.
    /// @param funcName Enclosing function name used to qualify the label.
    /// @param name Canonical block label.
    /// @return Local label value spelling @c ".L_<function>_<block>".
    static ILValue makeBlockLabelValue(const std::string &funcName, std::string name) {
        return makeLabelValue(".L_" + funcName + "_" + name);
    }

    /// @brief Create an immediate adapter value storing a condition code.
    /// @param code Backend condition-code integer.
    /// @return Non-SSA I64 immediate containing the wrapped 64-bit value.
    static ILValue makeCondImmediate(int code) {
        ILValue imm{};
        imm.kind = ILValue::Kind::I64;
        imm.i64 = il::common::integer::narrow_to(
            static_cast<long long>(code), 64, il::common::integer::OverflowPolicy::Wrap);
        imm.id = -1;
        return imm;
    }

    /// @brief Translate IL comparison opcodes into backend condition codes.
    /// @param op Supported integer or ordered floating-point comparison opcode.
    /// @return Backend condition code in the inclusive range 0 through 9.
    /// @throws std::runtime_error If @p op is not one of the mapped predicates.
    static int condCodeFor(il::core::Opcode op) {
        using il::core::Opcode;
        switch (op) {
            case Opcode::ICmpEq:
            case Opcode::FCmpEQ:
                return 0;
            case Opcode::ICmpNe:
            case Opcode::FCmpNE:
                return 1;
            case Opcode::SCmpLT:
            case Opcode::FCmpLT:
                return 2;
            case Opcode::SCmpLE:
            case Opcode::FCmpLE:
                return 3;
            case Opcode::SCmpGT:
            case Opcode::FCmpGT:
                return 4;
            case Opcode::SCmpGE:
            case Opcode::FCmpGE:
                return 5;
            case Opcode::UCmpGT:
                return 6;
            case Opcode::UCmpGE:
                return 7;
            case Opcode::UCmpLT:
                return 8;
            case Opcode::UCmpLE:
                return 9;
            default:
                throw std::runtime_error("x86-64 lowering: unsupported comparison opcode");
        }
    }

    //-------------------------------------------------------------------------
    // Value Conversion
    //-------------------------------------------------------------------------

    /// @brief Convert an IL operand into the backend adapter value.
    /// @details Resolves temporary kinds and widths from the current function's
    ///          SSA maps, copies literal payloads, and represents global
    ///          addresses as labels. A hint overrides the class of non-temp,
    ///          non-label values so overloaded operands enter the intended
    ///          register bank.
    /// @param value Canonical operand to convert.
    /// @param hint Optional expected adapter kind.
    /// @return Fully populated adapter operand, using id -1 for non-SSA values.
    /// @throws std::runtime_error If a temporary lacks registered kind metadata.
    ILValue convertValue(const il::core::Value &value, std::optional<ILValue::Kind> hint) {
        ILValue converted{};
        converted.id = -1;

        switch (value.kind) {
            case il::core::Value::Kind::Temp: {
                const auto it = valueKinds_.find(value.id);
                if (it == valueKinds_.end()) {
                    std::string detail = "ssa temp %" + std::to_string(value.id) +
                                         " without registered kind in Phase A lowering";
                    if (currentFunc_)
                        detail += " in function @" + currentFunc_->name;
                    if (currentBlock_)
                        detail += " block %" + currentBlock_->label;
                    reportUnsupported(std::move(detail));
                }
                converted.kind = it->second;
                converted.id = static_cast<int>(value.id);
                if (const auto bitsIt = valueBits_.find(value.id); bitsIt != valueBits_.end()) {
                    converted.bits = bitsIt->second;
                }
                break;
            }
            case il::core::Value::Kind::ConstInt: {
                converted.kind =
                    hint.value_or(value.isBool ? ILValue::Kind::I1 : ILValue::Kind::I64);
                converted.bits = value.isBool ? 1 : 64;
                converted.i64 = value.i64;
                break;
            }
            case il::core::Value::Kind::ConstFloat:
                converted.kind = ILValue::Kind::F64;
                converted.f64 = value.f64;
                break;
            case il::core::Value::Kind::ConstStr:
                converted.kind = ILValue::Kind::STR;
                converted.str = value.str;
                converted.strLen = static_cast<std::uint64_t>(value.str.size());
                break;
            case il::core::Value::Kind::GlobalAddr:
                converted.kind = ILValue::Kind::LABEL;
                converted.label = value.str;
                break;
            case il::core::Value::Kind::NullPtr:
                converted.kind = ILValue::Kind::PTR;
                converted.i64 = 0;
                break;
        }

        if (hint && value.kind != il::core::Value::Kind::Temp &&
            value.kind != il::core::Value::Kind::GlobalAddr) {
            converted.kind = *hint;
            converted.bits = *hint == ILValue::Kind::I1 ? 1 : 64;
        }

        return converted;
    }

    /// @brief Append converted operands to an adapter instruction.
    /// @details Applies hints positionally; operands beyond @p hints are
    ///          converted without a hint.
    /// @param instr Source instruction supplying operands.
    /// @param hints Expected kinds in source-operand order.
    /// @param out Destination adapter instruction whose operand list is extended.
    /// @throws std::runtime_error If operand conversion encounters invalid SSA metadata.
    void convertOperands(const il::core::Instr &instr,
                         std::initializer_list<std::optional<ILValue::Kind>> hints,
                         ILInstr &out) {
        std::size_t index = 0;
        for (const auto &operand : instr.operands) {
            const std::optional<ILValue::Kind> hint =
                index < hints.size() ? *(hints.begin() + static_cast<std::ptrdiff_t>(index))
                                     : std::optional<ILValue::Kind>{};
            out.ops.push_back(convertValue(operand, hint));
            ++index;
        }
    }

    //-------------------------------------------------------------------------
    // Result Registration
    //-------------------------------------------------------------------------

    /// @brief Record the kind associated with the instruction result.
    /// @details Updates both @p out and the adapter's SSA kind/width maps when
    ///          the source has a result id. A result-less instruction still
    ///          receives result metadata for later opcode selection.
    /// @param out Adapter instruction receiving result metadata.
    /// @param instr Source instruction supplying the optional SSA id.
    /// @param type Effective result type.
    /// @return Adapter kind corresponding to @p type.
    /// @throws std::runtime_error If @p type has no scalar adapter representation.
    ILValue::Kind setResultKind(ILInstr &out,
                                const il::core::Instr &instr,
                                const il::core::Type &type) {
        const ILValue::Kind kind = typeToKind(type);
        const std::uint8_t bits = typeBitWidth(type);
        if (instr.result) {
            out.resultId = static_cast<int>(*instr.result);
            out.resultKind = kind;
            out.resultBits = bits;
            valueKinds_[*instr.result] = kind;
            valueBits_[*instr.result] = bits;
        } else {
            out.resultKind = kind;
            out.resultBits = bits;
        }
        return kind;
    }

    /// @brief Set result kind to a fixed type (for bitwise ops that always produce I64).
    /// @param out Adapter instruction receiving result metadata.
    /// @param instr Source instruction supplying the optional SSA id.
    /// @param kind Backend-defined result kind; I1 records one bit and all
    ///        other kinds record 64 bits.
    void setFixedResultKind(ILInstr &out, const il::core::Instr &instr, ILValue::Kind kind) {
        if (instr.result) {
            out.resultId = static_cast<int>(*instr.result);
            out.resultKind = kind;
            out.resultBits = kind == ILValue::Kind::I1 ? 1 : 64;
            valueKinds_[*instr.result] = kind;
            valueBits_[*instr.result] = out.resultBits;
        } else {
            out.resultKind = kind;
            out.resultBits = kind == ILValue::Kind::I1 ? 1 : 64;
        }
    }

    //-------------------------------------------------------------------------
    // Function/Block Adaptation
    //-------------------------------------------------------------------------

    /// @brief Adapt an entire IL function.
    /// @details Resets per-function metadata, pre-registers parameter and result
    ///          kinds so textual block order does not affect SSA resolution,
    ///          then adapts blocks in source order.
    /// @param func Canonical function to convert.
    /// @return Adapter function preserving name, variadic flag, and block order.
    /// @throws std::runtime_error If any type, value, or instruction is unsupported.
    ILFunction adaptFunction(const il::core::Function &func) {
        currentFunc_ = &func;
        valueKinds_.clear();
        valueKinds_.reserve(func.valueNames.size() + func.params.size());
        valueBits_.clear();
        valueBits_.reserve(func.valueNames.size() + func.params.size());

        ILFunction adapted{};
        adapted.name = func.name;
        adapted.isVarArg = func.isVarArg;

        // Register parameter kinds
        for (const auto &param : func.params) {
            valueKinds_.emplace(param.id, typeToKind(param.type));
            valueBits_.emplace(param.id, typeBitWidth(param.type));
        }

        // Optimized IL can place a block before the block that declares an SSA
        // parameter it uses. Register every block parameter up front so lowering
        // depends on dominance, not textual block order.
        for (const auto &block : func.blocks) {
            for (const auto &param : block.params) {
                valueKinds_[param.id] = typeToKind(param.type);
                valueBits_[param.id] = typeBitWidth(param.type);
            }
        }

        // Instruction results can also be used in dominated blocks that appear
        // earlier in textual order. Pre-register result types so operand
        // conversion is independent of block layout.
        for (const auto &block : func.blocks) {
            for (const auto &instr : block.instructions) {
                if (!instr.result)
                    continue;
                const auto resultType = effectiveInstructionType(instr);
                if (resultType.kind == il::core::Type::Kind::Void)
                    continue;
                valueKinds_[*instr.result] = typeToKind(resultType);
                valueBits_[*instr.result] = typeBitWidth(resultType);
            }
        }

        // Adapt each block
        adapted.blocks.reserve(func.blocks.size());
        for (const auto &block : func.blocks) {
            adapted.blocks.push_back(adaptBlock(block));
        }

        return adapted;
    }

    /// @brief Adapt an IL block.
    /// @details Copies block-parameter ids and classes, converts instructions
    ///          in order, and temporarily installs the block for diagnostics.
    /// @param block Canonical basic block to convert.
    /// @return Adapter block with instructions and terminator-edge metadata.
    /// @throws std::runtime_error If a contained value or instruction is unsupported.
    ILBlock adaptBlock(const il::core::BasicBlock &block) {
        currentBlock_ = &block;
        ILBlock adapted{};
        adapted.name = block.label;

        // Register block parameter kinds
        adapted.paramIds.reserve(block.params.size());
        adapted.paramKinds.reserve(block.params.size());
        for (const auto &param : block.params) {
            const ILValue::Kind kind = typeToKind(param.type);
            adapted.paramIds.push_back(static_cast<int>(param.id));
            adapted.paramKinds.push_back(kind);
            valueKinds_[param.id] = kind;
            valueBits_[param.id] = typeBitWidth(param.type);
        }

        // Adapt each instruction
        for (const auto &instr : block.instructions) {
            adaptInstruction(instr, adapted);
        }

        currentBlock_ = nullptr;
        return adapted;
    }

    //-------------------------------------------------------------------------
    // Instruction Adaptation (by category)
    //-------------------------------------------------------------------------

    /// @brief Table-driven adapter for binary arithmetic / shift / bitwise.
    /// @details Each homogeneous cluster maps Opcode → fixed suffix string and
    ///          uses one of three adapter calls. Returning false leaves @p out
    ///          untouched and tells the dispatcher to keep matching.
    /// @param source Source instruction to classify.
    /// @param out Adapter instruction populated when a table matches.
    /// @return @c true for arithmetic, integer division/remainder, shift, or
    ///         bitwise opcodes; otherwise @c false.
    /// @throws std::runtime_error If matched-instruction operands or types are invalid.
    bool tryAdaptArithLike(const il::core::Instr &source, ILInstr &out) {
        struct Entry {
            il::core::Opcode op{};
            const char *name{nullptr};
        };

        static constexpr std::array<Entry, 9> kArith = {{
            {il::core::Opcode::Add, "add"},
            {il::core::Opcode::FAdd, "add"},
            {il::core::Opcode::IAddOvf, "iadd.ovf"},
            {il::core::Opcode::Sub, "sub"},
            {il::core::Opcode::FSub, "sub"},
            {il::core::Opcode::ISubOvf, "isub.ovf"},
            {il::core::Opcode::Mul, "mul"},
            {il::core::Opcode::FMul, "mul"},
            {il::core::Opcode::IMulOvf, "imul.ovf"},
        }};
        for (const auto &e : kArith)
            if (source.op == e.op) {
                adaptBinaryArithmetic(source, out, e.name);
                return true;
            }

        static constexpr std::array<Entry, 8> kIntDiv = {{
            {il::core::Opcode::SDiv, "sdiv"},
            {il::core::Opcode::SDivChk0, "sdiv.chk0"},
            {il::core::Opcode::SRem, "srem"},
            {il::core::Opcode::SRemChk0, "srem.chk0"},
            {il::core::Opcode::UDiv, "udiv"},
            {il::core::Opcode::UDivChk0, "udiv.chk0"},
            {il::core::Opcode::URem, "urem"},
            {il::core::Opcode::URemChk0, "urem.chk0"},
        }};
        for (const auto &e : kIntDiv)
            if (source.op == e.op) {
                adaptIntDiv(source, out, e.name);
                return true;
            }

        static constexpr std::array<Entry, 3> kShift = {{
            {il::core::Opcode::Shl, "shl"},
            {il::core::Opcode::LShr, "lshr"},
            {il::core::Opcode::AShr, "ashr"},
        }};
        for (const auto &e : kShift)
            if (source.op == e.op) {
                adaptShift(source, out, e.name);
                return true;
            }

        static constexpr std::array<Entry, 3> kBitwise = {{
            {il::core::Opcode::And, "and"},
            {il::core::Opcode::Or, "or"},
            {il::core::Opcode::Xor, "xor"},
        }};
        for (const auto &e : kBitwise)
            if (source.op == e.op) {
                adaptBitwise(source, out, e.name);
                return true;
            }

        return false;
    }

    /// @brief Table-driven adapter for integer and float comparison clusters.
    /// @param source Source instruction to classify.
    /// @param out Adapter instruction populated when a table matches.
    /// @return @c true for a supported comparison opcode; otherwise @c false.
    /// @throws std::runtime_error If matched-instruction operands or metadata are invalid.
    bool tryAdaptCompare(const il::core::Instr &source, ILInstr &out) {
        static constexpr std::array<il::core::Opcode, 10> kIntCmp = {
            il::core::Opcode::ICmpEq,
            il::core::Opcode::ICmpNe,
            il::core::Opcode::SCmpLT,
            il::core::Opcode::SCmpLE,
            il::core::Opcode::SCmpGT,
            il::core::Opcode::SCmpGE,
            il::core::Opcode::UCmpGT,
            il::core::Opcode::UCmpGE,
            il::core::Opcode::UCmpLT,
            il::core::Opcode::UCmpLE,
        };
        for (auto op : kIntCmp)
            if (source.op == op) {
                adaptIntCompare(source, out);
                return true;
            }

        struct FpEntry {
            il::core::Opcode op{};
            const char *name{nullptr};
        };

        static constexpr std::array<FpEntry, 8> kFpCmp = {{
            {il::core::Opcode::FCmpEQ, "fcmp_eq"},
            {il::core::Opcode::FCmpNE, "fcmp_ne"},
            {il::core::Opcode::FCmpLT, "fcmp_lt"},
            {il::core::Opcode::FCmpLE, "fcmp_le"},
            {il::core::Opcode::FCmpGT, "fcmp_gt"},
            {il::core::Opcode::FCmpGE, "fcmp_ge"},
            {il::core::Opcode::FCmpOrd, "fcmp_ord"},
            {il::core::Opcode::FCmpUno, "fcmp_uno"},
        }};
        for (const auto &e : kFpCmp)
            if (source.op == e.op) {
                adaptFloatCompareAs(source, out, e.name);
                return true;
            }

        return false;
    }

    /// @brief Table-driven adapter for runtime-trampoline IL opcodes.
    /// @details Most error-handling intrinsics lower to a simple call into a
    ///          runtime helper. Opcode-specific hint lists drive operand
    ///          conversion when the helper expects typed arguments.
    /// @param source Source instruction to classify.
    /// @param out Adapter call populated when a runtime mapping matches.
    /// @return @c true for a mapped trap/error opcode; otherwise @c false.
    /// @throws std::runtime_error If matched-instruction operands or metadata are invalid.
    bool tryAdaptRuntimeCall(const il::core::Instr &source, ILInstr &out) {
        using HintList = std::initializer_list<std::optional<ILValue::Kind>>;
        switch (source.op) {
            case il::core::Opcode::TrapKind:
            case il::core::Opcode::ErrGetKind:
                adaptRuntimeCall(source, out, "rt_trap_get_kind");
                return true;
            case il::core::Opcode::TrapErr:
                adaptRuntimeCall(source,
                                 out,
                                 "rt_trap_error_make",
                                 HintList{ILValue::Kind::I64, ILValue::Kind::STR});
                return true;
            case il::core::Opcode::ErrGetCode:
                adaptRuntimeCall(source, out, "rt_trap_get_code");
                return true;
            case il::core::Opcode::ErrGetIp:
                adaptRuntimeCall(source, out, "rt_trap_get_ip");
                return true;
            case il::core::Opcode::ErrGetLine:
                adaptRuntimeCall(source, out, "rt_trap_get_line");
                return true;
            case il::core::Opcode::ErrGetMsg:
                adaptRuntimeCall(source, out, "rt_throw_msg_get");
                return true;
            case il::core::Opcode::TrapFromErr:
                adaptRuntimeCall(source, out, "rt_trap_raise_error", HintList{ILValue::Kind::I64});
                return true;
            default:
                return false;
        }
    }

    /// @brief Adapt a single instruction and append to block.
    /// @details Replaces an eligible void placeholder type with its inferred
    ///          integer type, dispatches the opcode to a family adapter, and
    ///          appends exactly one adapter instruction on success.
    /// @param instr Canonical instruction to convert.
    /// @param block Destination adapter block.
    /// @throws std::runtime_error If the opcode is unsupported or malformed.
    void adaptInstruction(const il::core::Instr &instr, ILBlock &block) {
        il::core::Instr typedInstr = instr;
        typedInstr.type = effectiveInstructionType(instr);
        const il::core::Instr &source = typedInstr;

        ILInstr out{};
        out.resultId = -1;
        out.loc = source.loc;

        // Fast paths for homogeneous opcode clusters.
        if (tryAdaptArithLike(source, out) || tryAdaptCompare(source, out) ||
            tryAdaptRuntimeCall(source, out)) {
            block.instrs.push_back(std::move(out));
            return;
        }

        switch (source.op) {
            case il::core::Opcode::FDiv:
                adaptFDiv(source, out);
                break;

            // Call
            case il::core::Opcode::Call:
                adaptCall(source, out);
                break;
            case il::core::Opcode::CallIndirect:
                adaptCallIndirect(source, out);
                break;

            // Exception handling
            case il::core::Opcode::EhPush:
                adaptEhPush(source, out);
                break;
            case il::core::Opcode::EhPop:
                adaptEhPop(out);
                break;
            case il::core::Opcode::EhEntry:
                adaptEhEntry(out);
                break;

            // Memory operations
            case il::core::Opcode::Load:
                adaptLoad(source, out);
                break;
            case il::core::Opcode::Store:
                adaptStore(source, out);
                break;

            // Cast operations
            case il::core::Opcode::Zext1:
                adaptZext(source, out);
                break;
            case il::core::Opcode::Trunc1:
                adaptTrunc(source, out);
                break;
            case il::core::Opcode::CastSiToFp:
            case il::core::Opcode::Sitofp:
                adaptSiToFp(source, out);
                break;
            case il::core::Opcode::CastFpToSiRteChk:
                adaptFpToSiChecked(source, out);
                break;
            case il::core::Opcode::Fptosi:
                adaptFpToSi(source, out);
                break;
            case il::core::Opcode::CastFpToUiRteChk:
                adaptFpToUi(source, out);
                break;
            case il::core::Opcode::CastUiToFp:
                adaptUiToFp(source, out);
                break;
            case il::core::Opcode::CastSiNarrowChk:
            case il::core::Opcode::CastUiNarrowChk:
                adaptNarrowCast(source, out);
                break;

            // Control flow
            case il::core::Opcode::Ret:
                adaptRet(source, out);
                break;
            case il::core::Opcode::Br:
                adaptBr(source, out, block);
                break;
            case il::core::Opcode::CBr:
                adaptCBr(source, out, block);
                break;
            case il::core::Opcode::Trap:
                out.opcode = "trap";
                convertOperands(source, {std::nullopt}, out);
                break;

            // String operations
            case il::core::Opcode::ConstStr:
                adaptConstStr(source, out);
                break;

            // Constants and addresses
            case il::core::Opcode::ConstNull:
                adaptConstNull(source, out);
                break;
            case il::core::Opcode::ConstF64:
                adaptConstF64(source, out);
                break;
            case il::core::Opcode::GAddr:
                adaptGAddr(source, out);
                break;
            case il::core::Opcode::AddrOf:
                adaptAddrOf(source, out);
                break;

            // Memory allocation
            case il::core::Opcode::Alloca:
                adaptAlloca(source, out);
                break;
            case il::core::Opcode::GEP:
                adaptGEP(source, out);
                break;

            // Bounds checking
            case il::core::Opcode::IdxChk:
                adaptIdxChk(source, out);
                break;

            // Value selection
            case il::core::Opcode::Select:
                adaptSelect(source, out);
                break;

            // Switch
            case il::core::Opcode::SwitchI32:
                adaptSwitchI32(source, out, block);
                break;

            // resume.label is a branch to an explicit target label;
            // the resume token operand is ignored in native codegen.
            case il::core::Opcode::ResumeLabel:
                adaptBr(source, out, block);
                break;

            case il::core::Opcode::ResumeSame:
            case il::core::Opcode::ResumeNext:
                reportUnsupported(std::string{"x86-64 lowering received raw "} +
                                  il::core::toString(source.op) +
                                  " after NativeEHLowering; structured EH rewrite is incomplete.");

            default:
                reportUnsupported(std::string{"IL opcode '"} + il::core::toString(source.op) +
                                  "' not supported by x86-64 Phase A");
        }

        block.instrs.push_back(std::move(out));
    }

    //-------------------------------------------------------------------------
    // Arithmetic Instruction Adapters
    //-------------------------------------------------------------------------

    /// @brief Adapt any binary arithmetic IL instruction.
    /// @details Records the result kind and hints both operands with the
    ///          same kind so integer/float overloads route correctly.
    /// @param instr Source IL instruction.
    /// @param out Adapter instruction being populated.
    /// @param opcode Adapter opcode string (e.g. "add", "iadd.ovf").
    /// @throws std::runtime_error If result type or operand metadata is unsupported.
    void adaptBinaryArithmetic(const il::core::Instr &instr, ILInstr &out, const char *opcode) {
        const ILValue::Kind kind = setResultKind(out, instr, instr.type);
        out.opcode = opcode;
        convertOperands(instr, {kind, kind}, out);
    }

    /// @brief Adapt the floating-point division IL instruction.
    /// @details Hard-codes the F64 result kind because @c fdiv is exclusively
    ///          a floating-point operation in IL.
    /// @param instr Source floating-point division instruction.
    /// @param out Adapter instruction receiving result and F64 operands.
    void adaptFDiv(const il::core::Instr &instr, ILInstr &out) {
        out.opcode = "fdiv";
        out.resultKind = ILValue::Kind::F64;
        if (instr.result) {
            out.resultId = static_cast<int>(*instr.result);
            valueKinds_[*instr.result] = ILValue::Kind::F64;
        }
        convertOperands(instr, {ILValue::Kind::F64, ILValue::Kind::F64}, out);
    }

    /// @brief Adapt an integer div/rem family instruction.
    /// @details Both operands are forced to I64 because IDIV/DIV consume the
    ///          full 64-bit register pair. Variant selection (signed vs.
    ///          unsigned, checked vs. plain) is encoded in @p opcode.
    /// @param instr Source integer division or remainder instruction.
    /// @param out Adapter instruction being populated.
    /// @param opcode Adapter opcode spelling identifying signedness and checks.
    /// @throws std::runtime_error If result type or operands are unsupported.
    void adaptIntDiv(const il::core::Instr &instr, ILInstr &out, const char *opcode) {
        setResultKind(out, instr, instr.type);
        out.opcode = opcode;
        convertOperands(instr, {ILValue::Kind::I64, ILValue::Kind::I64}, out);
    }

    /// @brief Adapt a shift IL instruction.
    /// @details The shifted value follows the result kind so 32-bit shifts
    ///          do not silently widen; the shift count is always I64.
    /// @param instr Source shift instruction.
    /// @param out Adapter instruction being populated.
    /// @param opcode Adapter shift opcode spelling.
    /// @throws std::runtime_error If result type or operands are unsupported.
    void adaptShift(const il::core::Instr &instr, ILInstr &out, const char *opcode) {
        const ILValue::Kind kind = setResultKind(out, instr, instr.type);
        out.opcode = opcode;
        convertOperands(instr, {kind, ILValue::Kind::I64}, out);
    }

    /// @brief Adapt a bitwise opcode (AND / OR / XOR).
    /// @details Result is always pinned to I64 because IL bitwise ops are
    ///          defined over the full 64-bit register width regardless of
    ///          declared type.
    /// @param instr Source bitwise instruction.
    /// @param out Adapter instruction being populated.
    /// @param opcode Adapter opcode spelling.
    /// @throws std::runtime_error If operand SSA metadata is invalid.
    void adaptBitwise(const il::core::Instr &instr, ILInstr &out, const char *opcode) {
        setFixedResultKind(out, instr, ILValue::Kind::I64);
        out.opcode = opcode;
        convertOperands(instr, {ILValue::Kind::I64, ILValue::Kind::I64}, out);
    }

    //-------------------------------------------------------------------------
    // Comparison Instruction Adapters
    //-------------------------------------------------------------------------

    /// @brief Adapt an integer compare IL instruction.
    /// @details Emits a "cmp" opcode with an extra trailing immediate that
    ///          encodes the SETcc condition code derived from the IL opcode.
    ///          The result is always I1.
    /// @param instr Source integer comparison.
    /// @param out Adapter instruction receiving two I64 operands and condition code.
    /// @throws std::runtime_error If the predicate or operand metadata is unsupported.
    void adaptIntCompare(const il::core::Instr &instr, ILInstr &out) {
        out.opcode = "cmp";
        setFixedResultKind(out, instr, ILValue::Kind::I1);
        convertOperands(instr, {ILValue::Kind::I64, ILValue::Kind::I64}, out);
        out.ops.push_back(makeCondImmediate(condCodeFor(instr.op)));
    }

    /// @brief Adapt a floating-point compare IL instruction.
    /// @details The adapter opcode (@p opcode) carries the specific
    ///          predicate suffix (e.g. "fcmp_lt") so the rule table can
    ///          select the right NaN-safe sequence.
    /// @param instr Source floating-point comparison.
    /// @param out Adapter instruction receiving two F64 operands.
    /// @param opcode Predicate-specific adapter opcode spelling.
    /// @throws std::runtime_error If operand SSA metadata is invalid.
    void adaptFloatCompareAs(const il::core::Instr &instr, ILInstr &out, const char *opcode) {
        out.opcode = opcode;
        setFixedResultKind(out, instr, ILValue::Kind::I1);
        convertOperands(instr, {ILValue::Kind::F64, ILValue::Kind::F64}, out);
    }

    //-------------------------------------------------------------------------
    // Call Instruction Adapter
    //-------------------------------------------------------------------------

    /// @brief Adapt a direct call IL instruction.
    /// @details The callee name is materialised as the first operand (a label
    ///          @c ILValue) and the IL arg list follows. Void-returning calls
    ///          that nevertheless carry a result id are rejected as malformed.
    /// @param instr Source direct call with callee name and arguments.
    /// @param out Adapter call instruction being populated.
    /// @throws std::runtime_error For a void call carrying an SSA result or an
    ///         argument with invalid metadata.
    void adaptCall(const il::core::Instr &instr, ILInstr &out) {
        if (instr.type.kind != il::core::Type::Kind::Void) {
            setResultKind(out, instr, instr.type);
        } else if (instr.result) {
            reportUnsupported(std::string{"call to '"} + instr.callee +
                              "' has a result SSA id but void return type");
        }
        out.opcode = "call";
        out.ops.push_back(makeLabelValue(instr.callee));
        for (const auto &operand : instr.operands) {
            out.ops.push_back(convertValue(operand, std::nullopt));
        }
    }

    /// @brief Adapt an indirect call IL instruction.
    /// @details Unlike @ref adaptCall the first operand is a runtime
    ///          pointer (not a label). The backend's @c emitCallIndirect
    ///          materialises it into a register before issuing CALL.
    /// @param instr Source indirect call whose first operand is the target.
    /// @param out Adapter call instruction being populated.
    /// @throws std::runtime_error For a void call carrying an SSA result or an
    ///         operand with invalid metadata.
    void adaptCallIndirect(const il::core::Instr &instr, ILInstr &out) {
        if (instr.type.kind != il::core::Type::Kind::Void) {
            setResultKind(out, instr, instr.type);
        } else if (instr.result) {
            reportUnsupported("call.indirect has a result SSA id but void return type");
        }
        out.opcode = "call.indirect";
        for (const auto &operand : instr.operands) {
            out.ops.push_back(convertValue(operand, std::nullopt));
        }
    }

    /// @brief Adapt an IL opcode that lowers to a runtime call.
    /// @details Used for opcodes like string operations that have no
    ///          one-to-one MIR opcode and instead invoke a runtime helper.
    ///          The runtime callee name is supplied explicitly.
    /// @param instr Source IL instruction.
    /// @param out Adapter instruction being populated.
    /// @param callee Fully qualified runtime symbol to dispatch to.
    /// @param hints Per-argument kind hints used to coerce operand classes.
    /// @throws std::runtime_error If result or operand metadata is unsupported.
    void adaptRuntimeCall(const il::core::Instr &instr,
                          ILInstr &out,
                          const char *callee,
                          std::initializer_list<std::optional<ILValue::Kind>> hints = {}) {
        if (instr.type.kind != il::core::Type::Kind::Void) {
            setResultKind(out, instr, instr.type);
        }
        out.opcode = "call";
        out.ops.push_back(makeLabelValue(callee));
        std::size_t index = 0;
        for (const auto &operand : instr.operands) {
            const std::optional<ILValue::Kind> hint =
                index < hints.size() ? *(hints.begin() + static_cast<std::ptrdiff_t>(index))
                                     : std::optional<ILValue::Kind>{};
            out.ops.push_back(convertValue(operand, hint));
            ++index;
        }
    }

    //-------------------------------------------------------------------------
    // Constants and Addresses
    //-------------------------------------------------------------------------

    /// @brief Adapt the IL `const_null` opcode (null pointer literal).
    /// @param instr Source instruction supplying the optional result id.
    /// @param out Adapter pointer-producing instruction being populated.
    void adaptConstNull(const il::core::Instr &instr, ILInstr &out) {
        setFixedResultKind(out, instr, ILValue::Kind::PTR);
        out.opcode = "const_null";
    }

    /// @brief Adapt the IL `const_f64` opcode (double literal).
    /// @param instr Source instruction supplying result and literal operand.
    /// @param out Adapter F64-producing instruction being populated.
    /// @throws std::runtime_error If operand SSA metadata is invalid.
    void adaptConstF64(const il::core::Instr &instr, ILInstr &out) {
        setFixedResultKind(out, instr, ILValue::Kind::F64);
        out.opcode = "const_f64";
        convertOperands(instr, {ILValue::Kind::F64}, out);
    }

    /// @brief Adapt the IL `gaddr` opcode (global address materialisation).
    /// @param instr Source global-address instruction.
    /// @param out Adapter pointer-producing instruction being populated.
    /// @throws std::runtime_error If operand SSA metadata is invalid.
    void adaptGAddr(const il::core::Instr &instr, ILInstr &out) {
        setFixedResultKind(out, instr, ILValue::Kind::PTR);
        out.opcode = "gaddr";
        convertOperands(instr, {std::nullopt}, out);
    }

    /// @brief Adapt the IL `addr_of` opcode (address of an alloca slot).
    /// @param instr Source address-of instruction.
    /// @param out Adapter pointer-producing instruction being populated.
    /// @throws std::runtime_error If operand SSA metadata is invalid.
    void adaptAddrOf(const il::core::Instr &instr, ILInstr &out) {
        setFixedResultKind(out, instr, ILValue::Kind::PTR);
        out.opcode = "addr_of";
        convertOperands(instr, {ILValue::Kind::PTR}, out);
    }

    //-------------------------------------------------------------------------
    // Bounds Check and Switch
    //-------------------------------------------------------------------------

    /// @brief Adapt the IL `idx_chk` opcode (index bounds check + normalise).
    /// @details Three I64 operands: index, lower bound, upper bound.
    /// @param instr Source index-check instruction.
    /// @param out Adapter instruction being populated.
    /// @throws std::runtime_error If result type or operand metadata is unsupported.
    void adaptIdxChk(const il::core::Instr &instr, ILInstr &out) {
        setResultKind(out, instr, instr.type);
        out.opcode = "idx_chk";
        convertOperands(instr, {ILValue::Kind::I64, ILValue::Kind::I64, ILValue::Kind::I64}, out);
    }

    /// @brief Adapt the IL `select` opcode (ternary value choice).
    /// @details The first operand is the condition and the remaining operands
    ///          are hinted with the instruction's result kind, which fixes GPR
    ///          versus XMM lowering downstream.
    /// @param instr Source ternary select.
    /// @param out Adapter instruction being populated.
    /// @throws std::runtime_error If result type or operand metadata is unsupported.
    void adaptSelect(const il::core::Instr &instr, ILInstr &out) {
        setResultKind(out, instr, instr.type);
        out.opcode = "select";
        const ILValue::Kind armKind = out.resultKind;
        convertOperands(instr, {ILValue::Kind::I64, armKind, armKind}, out);
    }

    /// @brief Adapt the IL `switch_i32` opcode.
    /// @details Re-encodes the IL successor table as interleaved
    ///          (case-value, case-label) pairs followed by the default
    ///          label. Also records terminator edges so block-parameter
    ///          copies route correctly per arm.
    /// @param instr Source switch instruction and successor tables.
    /// @param out Adapter switch instruction being populated.
    /// @param block Destination block receiving edge-argument records.
    /// @pre A current function is active for block-label qualification.
    /// @throws std::runtime_error If a case value or edge argument is unsupported.
    void adaptSwitchI32(const il::core::Instr &instr, ILInstr &out, ILBlock &block) {
        using namespace il::core;
        out.opcode = "switch_i32";
        // Operand 0: scrutinee
        if (!instr.operands.empty()) {
            out.ops.push_back(convertValue(instr.operands[0], ILValue::Kind::I64));
        }
        // Case values and labels interleaved: [value0, label0, value1, label1, ...]
        const std::size_t ncases = switchCaseCount(instr);
        for (std::size_t ci = 0; ci < ncases; ++ci) {
            const Value &cval = switchCaseValue(instr, ci);
            out.ops.push_back(convertValue(cval, ILValue::Kind::I64));
            out.ops.push_back(makeBlockLabelValue(currentFunc_->name, switchCaseLabel(instr, ci)));
        }
        // Default label as last operand
        const std::string &defLabel = switchDefaultLabel(instr);
        if (!defLabel.empty()) {
            out.ops.push_back(makeBlockLabelValue(currentFunc_->name, defLabel));
        }
        // Add terminator edges for block argument passing
        addTerminatorEdges(instr, block);
    }

    //-------------------------------------------------------------------------
    // Exception Handling Adapters
    //-------------------------------------------------------------------------

    /// @brief Adapt the IL `eh.push` opcode (push EH handler frame).
    /// @details Trimmed to a single operand because @c NativeEHLowering
    ///          rewrites the structured marker before MIR lowering; the
    ///          fallback only needs to preserve the handler label.
    /// @param instr Residual handler-frame marker.
    /// @param out Adapter marker being populated.
    /// @throws std::runtime_error If operand metadata is invalid.
    void adaptEhPush(const il::core::Instr &instr, ILInstr &out) {
        out.opcode = "eh.push";
        convertOperands(instr, {std::nullopt}, out);
        if (out.ops.size() > 1) {
            out.ops.resize(1);
        }
    }

    /// @brief Adapt the IL `eh.pop` opcode.
    /// @details Inert in the MIR pipeline; NativeEHLowering will have
    ///          rewritten it before lowering.
    /// @param out Adapter marker receiving the inert opcode and placeholder kind.
    void adaptEhPop(ILInstr &out) {
        out.opcode = "eh.pop";
        out.resultKind = ILValue::Kind::I64; // unused
    }

    /// @brief Adapt the IL `eh.entry` handler-block marker.
    /// @details Inert fallback like @ref adaptEhPop.
    /// @param out Adapter marker receiving the inert opcode and placeholder kind.
    void adaptEhEntry(ILInstr &out) {
        out.opcode = "eh.entry";
        out.resultKind = ILValue::Kind::I64; // unused
    }

    //-------------------------------------------------------------------------
    // Memory Operation Adapters
    //-------------------------------------------------------------------------

    /// @brief Adapt the IL `load` opcode.
    /// @details Operands: pointer (PTR) + optional displacement (I64).
    ///          Any extra operands are trimmed because the MIR `load`
    ///          shape is fixed at two arguments.
    /// @param instr Source load and effective result type.
    /// @param out Adapter load instruction being populated.
    /// @throws std::runtime_error If result type or operand metadata is unsupported.
    void adaptLoad(const il::core::Instr &instr, ILInstr &out) {
        setResultKind(out, instr, instr.type);
        out.opcode = "load";
        convertOperands(instr, {ILValue::Kind::PTR, ILValue::Kind::I64}, out);
        if (out.ops.size() > 2) {
            out.ops.resize(2);
        }
    }

    /// @brief Adapt the IL `store` opcode.
    /// @details Operands: pointer, value (typed to match the stored width),
    ///          optional displacement. The third operand is dropped when
    ///          absent so MIR opcode validators see a uniform shape.
    /// @param instr Source store whose type describes the stored value.
    /// @param out Adapter store instruction being populated.
    /// @throws std::runtime_error If stored type or operand metadata is unsupported.
    void adaptStore(const il::core::Instr &instr, ILInstr &out) {
        out.opcode = "store";
        out.resultBits = typeBitWidth(instr.type);
        convertOperands(
            instr, {ILValue::Kind::PTR, typeToKind(instr.type), ILValue::Kind::I64}, out);
        if (out.ops.size() > 3) {
            out.ops.resize(3);
        }
    }

    /// @brief Adapt alloca instruction for stack allocation.
    /// @param instr Source allocation with a byte-count operand.
    /// @param out Adapter pointer-producing allocation instruction.
    /// @throws std::runtime_error If operand metadata is invalid.
    void adaptAlloca(const il::core::Instr &instr, ILInstr &out) {
        setFixedResultKind(out, instr, ILValue::Kind::PTR);
        out.opcode = "alloca";
        // Operand 0 is the size in bytes
        convertOperands(instr, {ILValue::Kind::I64}, out);
    }

    /// @brief Adapt GEP (get element pointer) instruction.
    /// @param instr Source pointer-plus-byte-offset instruction.
    /// @param out Adapter pointer-producing address instruction.
    /// @throws std::runtime_error If operand metadata is invalid.
    void adaptGEP(const il::core::Instr &instr, ILInstr &out) {
        setFixedResultKind(out, instr, ILValue::Kind::PTR);
        out.opcode = "gep";
        // Operand 0 is the base pointer, operand 1 is the byte offset
        convertOperands(instr, {ILValue::Kind::PTR, ILValue::Kind::I64}, out);
    }

    //-------------------------------------------------------------------------
    // Cast Operation Adapters
    //-------------------------------------------------------------------------

    /// @brief Adapt the IL `zext` opcode (boolean to integer extension).
    /// @param instr Source extension and destination type.
    /// @param out Adapter cast instruction being populated.
    /// @throws std::runtime_error If result type or operand metadata is unsupported.
    void adaptZext(const il::core::Instr &instr, ILInstr &out) {
        setResultKind(out, instr, instr.type);
        out.opcode = "zext";
        convertOperands(instr, {ILValue::Kind::I1}, out);
    }

    /// @brief Adapt the IL `trunc` opcode (integer width narrowing).
    /// @param instr Source truncation and destination type.
    /// @param out Adapter cast instruction being populated.
    /// @throws std::runtime_error If result type or operand metadata is unsupported.
    void adaptTrunc(const il::core::Instr &instr, ILInstr &out) {
        setResultKind(out, instr, instr.type);
        out.opcode = "trunc";
        convertOperands(instr, {ILValue::Kind::I64}, out);
    }

    /// @brief Adapt the IL `sitofp` opcode (signed int to double).
    /// @param instr Source conversion and destination type.
    /// @param out Adapter cast instruction being populated.
    /// @throws std::runtime_error If result type or operand metadata is unsupported.
    void adaptSiToFp(const il::core::Instr &instr, ILInstr &out) {
        setResultKind(out, instr, instr.type);
        out.opcode = "sitofp";
        convertOperands(instr, {ILValue::Kind::I64}, out);
    }

    /// @brief Adapt the IL `fptosi` opcode (truncating fp-to-signed-int).
    /// @param instr Source conversion and destination type.
    /// @param out Adapter cast instruction being populated.
    /// @throws std::runtime_error If result type or operand metadata is unsupported.
    void adaptFpToSi(const il::core::Instr &instr, ILInstr &out) {
        setResultKind(out, instr, instr.type);
        out.opcode = "fptosi";
        convertOperands(instr, {ILValue::Kind::F64}, out);
    }

    /// @brief Adapt the IL `fptosi.chk` opcode (rounded, range-checked).
    /// @param instr Source checked conversion and destination type.
    /// @param out Adapter cast instruction being populated.
    /// @throws std::runtime_error If result type or operand metadata is unsupported.
    void adaptFpToSiChecked(const il::core::Instr &instr, ILInstr &out) {
        setResultKind(out, instr, instr.type);
        out.opcode = "fptosi_chk";
        convertOperands(instr, {ILValue::Kind::F64}, out);
    }

    /// @brief Adapt the IL `fptoui` opcode (fp-to-unsigned-int with checks).
    /// @param instr Source conversion and destination type.
    /// @param out Adapter cast instruction being populated.
    /// @throws std::runtime_error If result type or operand metadata is unsupported.
    void adaptFpToUi(const il::core::Instr &instr, ILInstr &out) {
        setResultKind(out, instr, instr.type);
        out.opcode = "fptoui";
        convertOperands(instr, {ILValue::Kind::F64}, out);
    }

    /// @brief Adapt the IL `uitofp` opcode (unsigned int to double).
    /// @param instr Source conversion and destination type.
    /// @param out Adapter cast instruction being populated.
    /// @throws std::runtime_error If result type or operand metadata is unsupported.
    void adaptUiToFp(const il::core::Instr &instr, ILInstr &out) {
        setResultKind(out, instr, instr.type);
        out.opcode = "uitofp";
        convertOperands(instr, {ILValue::Kind::I64}, out);
    }

    /// @brief Adapt narrowing cast (i64 -> i32 etc).
    /// @details Chooses the signed or unsigned checked adapter opcode from the
    ///          canonical source opcode.
    /// @param instr Source narrowing conversion and destination type.
    /// @param out Adapter cast instruction being populated.
    /// @throws std::runtime_error If result type or operand metadata is unsupported.
    void adaptNarrowCast(const il::core::Instr &instr, ILInstr &out) {
        setResultKind(out, instr, instr.type);
        out.opcode =
            instr.op == il::core::Opcode::CastSiNarrowChk ? "si_narrow_chk" : "ui_narrow_chk";
        convertOperands(instr, {ILValue::Kind::I64}, out);
    }

    //-------------------------------------------------------------------------
    // Control Flow Adapters
    //-------------------------------------------------------------------------

    /// @brief Adapt the IL `ret` opcode.
    /// @details Uses the enclosing function's declared return type to hint
    ///          the operand kind so I1 returns survive without surprise
    ///          widening.
    /// @param instr Source return with zero or one value operand.
    /// @param out Adapter return instruction being populated.
    /// @pre A current function is active.
    /// @throws std::runtime_error If return type or operand metadata is unsupported.
    void adaptRet(const il::core::Instr &instr, ILInstr &out) {
        out.opcode = "ret";
        if (!instr.operands.empty()) {
            const auto returnKind =
                currentFunc_->retType.kind == il::core::Type::Kind::Void
                    ? std::optional<ILValue::Kind>{}
                    : std::optional<ILValue::Kind>{typeToKind(currentFunc_->retType)};
            out.ops.push_back(convertValue(instr.operands.front(), returnKind));
        }
    }

    /// @brief Adapt the IL `br` (unconditional branch) opcode.
    /// @param instr Source branch, target label, and optional edge arguments.
    /// @param out Adapter branch instruction being populated.
    /// @param block Destination block receiving edge-argument records.
    /// @pre A current function is active when a target label is present.
    /// @throws std::runtime_error If an edge argument is unsupported.
    void adaptBr(const il::core::Instr &instr, ILInstr &out, ILBlock &block) {
        out.opcode = "br";
        if (!instr.labels.empty()) {
            out.ops.push_back(makeBlockLabelValue(currentFunc_->name, instr.labels.front()));
        }
        addTerminatorEdges(instr, block);
    }

    /// @brief Adapt the IL `cbr` (conditional branch) opcode.
    /// @details Operands: I1 condition, true-target label, false-target label.
    /// @param instr Source conditional branch and successor arguments.
    /// @param out Adapter branch instruction being populated.
    /// @param block Destination block receiving edge-argument records.
    /// @pre A current function is active for block-label qualification.
    /// @throws std::runtime_error If the condition is missing or an operand is unsupported.
    void adaptCBr(const il::core::Instr &instr, ILInstr &out, ILBlock &block) {
        out.opcode = "cbr";
        if (instr.operands.empty()) {
            reportUnsupported("conditional branch missing condition operand");
        }
        out.ops.push_back(convertValue(instr.operands.front(), ILValue::Kind::I1));
        for (const auto &label : instr.labels) {
            out.ops.push_back(makeBlockLabelValue(currentFunc_->name, label));
        }
        addTerminatorEdges(instr, block);
    }

    //-------------------------------------------------------------------------
    // String Operation Adapters
    //-------------------------------------------------------------------------

    /// @brief Adapt const_str instruction to produce a string value.
    /// @details Resolves the global-address operand against the current module
    ///          and embeds the referenced initializer bytes and length.
    /// @param instr Source string-constant instruction.
    /// @param out Adapter string-producing instruction being populated.
    /// @pre A current module is active.
    /// @throws std::runtime_error If the operand is absent, is not a global
    ///         address, or names an unknown global.
    void adaptConstStr(const il::core::Instr &instr, ILInstr &out) {
        setFixedResultKind(out, instr, ILValue::Kind::STR);
        out.opcode = "const_str";

        // The operand is a GlobalAddr pointing to the global string constant
        if (instr.operands.empty()) {
            reportUnsupported("const_str missing operand");
        }

        const auto &op = instr.operands.front();
        if (op.kind != il::core::Value::Kind::GlobalAddr) {
            reportUnsupported("const_str with non-global operand");
        }

        // Look up the global to get the actual string content
        const il::core::Global *global = findGlobal(op.str);
        if (!global) {
            reportUnsupported("const_str references unknown global: " + op.str);
        }

        // Create an ILValue with the string content
        ILValue strValue{};
        strValue.kind = ILValue::Kind::STR;
        strValue.str = global->init;
        strValue.strLen = static_cast<std::uint64_t>(global->init.size());
        strValue.id = -1;
        out.ops.push_back(strValue);
    }

    /// @brief Add terminator edges for branch instructions.
    /// @details Handles both SSA temps (via SSA id) and constants (via ILValue
    ///          materialization in emitEdgeCopies). For constants, a sentinel -1
    ///          is stored in argIds and the full ILValue is stored in argValues.
    /// @param instr Terminator supplying labels and per-successor argument lists.
    /// @param block Adapter block whose edge vector is extended.
    /// @throws std::runtime_error If an edge argument has invalid SSA metadata.
    void addTerminatorEdges(const il::core::Instr &instr, ILBlock &block) {
        const std::size_t succCount = instr.labels.size();
        block.terminatorEdges.reserve(block.terminatorEdges.size() + succCount);

        for (std::size_t idx = 0; idx < succCount; ++idx) {
            ILBlock::EdgeArg edge{};
            edge.to = instr.labels[idx];
            if (idx < instr.brArgs.size()) {
                for (const auto &arg : instr.brArgs[idx]) {
                    // Infer the kind hint for the block argument based on
                    // the destination block's parameter types.
                    std::optional<ILValue::Kind> hint;
                    if (arg.kind == il::core::Value::Kind::ConstInt) {
                        hint = arg.isBool ? ILValue::Kind::I1 : ILValue::Kind::I64;
                    }

                    const ILValue converted = convertValue(arg, hint);
                    edge.argValues.push_back(converted);

                    if (arg.kind == il::core::Value::Kind::Temp) {
                        edge.argIds.push_back(static_cast<int>(arg.id));
                    } else {
                        // Sentinel: constant to be materialized in emitEdgeCopies.
                        edge.argIds.push_back(-1);
                    }
                }
            }
            block.terminatorEdges.push_back(std::move(edge));
        }
    }
};

} // namespace

/// @brief Execute Phase A lowering for the provided pipeline module.
/// @details Catches unsupported-feature exceptions thrown by the adapter and
///          reports them through the diagnostics sink rather than propagating.
///          On success it replaces the adapter module, clears downstream MIR,
///          frame, text, read-only-data, and debug artifacts, resets stage
///          flags, and selects an ABI target if one is not already installed.
/// @param module Shared pipeline state whose lowered representation is replaced.
/// @param diags Diagnostic sink receiving caught adaptation failures.
/// @return @c true after complete adaptation and invalidation; @c false after
///         recording an exception message.
bool LoweringPass::run(Module &module, Diagnostics &diags) {
    try {
        ModuleAdapter adapter{};
        module.lowered = adapter.adapt(module.il);
        module.roData = AsmEmitter::RoDataPool{};
        module.mir.clear();
        module.frames.clear();
        module.codegenResult.reset();
        module.binaryText.reset();
        module.binaryRodata.reset();
        module.binaryTextSections.clear();
        module.debugLineData.clear();
        module.legalised = false;
        module.registersAllocated = false;
        if (!module.target)
            module.target = &selectTarget(module.options.targetABI);
        return true;
    } catch (const std::exception &e) {
        diags.error(std::string("lowering: ") + e.what());
        return false;
    }
}

} // namespace zanna::codegen::x64::passes
