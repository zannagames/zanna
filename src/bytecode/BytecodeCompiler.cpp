//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/bytecode/BytecodeCompiler.cpp
// Purpose: Lower verified Zanna IL modules into executable bytecode modules.
// Key invariants:
//   - Every emitted stack effect is mirrored by compile-time depth accounting.
//   - Function, global, local, branch, and pool indices fit their encodings.
//   - Branch and exception metadata is rebuilt only after target resolution.
// Ownership/Lifetime:
//   - The compiler owns its output and transient maps while borrowing input IL
//     and an optional SourceManager for the duration of compilation.
// Links: src/bytecode/BytecodeCompiler.hpp, src/bytecode/BytecodeSemantics.hpp,
//        docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/**
 * @file src/bytecode/BytecodeCompiler.cpp
 * @brief Implements checked lowering from IL to the stack-based bytecode format.
 *
 * @details
 * Lowering pre-registers symbols, assigns SSA values to flat locals, orders
 * basic blocks, emits and stack-checks bytecode, resolves relative branches,
 * and reconstructs exception and switch metadata. Expected input failures are
 * transported internally by `BytecodeCompileFailure` and exposed as structured
 * diagnostics at the `compileChecked()` boundary.
 */

#include "bytecode/BytecodeCompiler.hpp"
#include "bytecode/BytecodeSemantics.hpp"
#include "bytecode/BytecodeVM.hpp"
#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/OpcodeInfo.hpp"
#include "il/core/Value.hpp"
#include "il/verify/Verifier.hpp"
#include "support/source_manager.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <exception>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace zanna {
namespace bytecode {
namespace {
/**
 * @brief Internal exception carrying a structured bytecode compile diagnostic.
 *
 * The exception is confined to this translation unit and provides non-local
 * exit from deeply nested emit helpers without losing diagnostic metadata.
 */
class BytecodeCompileFailure final : public std::exception {
  public:
    /**
     * @brief Construct a failure by taking ownership of a diagnostic.
     * @param diag Diagnostic moved into exception storage.
     */
    explicit BytecodeCompileFailure(il::support::Diag diag) : diag_(std::move(diag)) {}

    /**
     * @brief Expose the diagnostic message through `std::exception`.
     * @return Pointer into the owned message string, valid until the exception
     *         is destroyed or its diagnostic is modified.
     */
    const char *what() const noexcept override {
        return diag_.message.c_str();
    }

    /**
     * @brief Access the complete structured diagnostic.
     * @return Borrowed reference valid for the exception lifetime.
     */
    const il::support::Diag &diag() const {
        return diag_;
    }

  private:
    il::support::Diag diag_;
};

/// @brief Byte size reserved for a global of the given IL type kind.
/// @details I1→1, I16→2, I32→4, Void→0; all 64-bit kinds (I64/F64/Ptr/Str/
///          Error/ResumeTok) and any unknown kind reserve 8 bytes.
/// @param kind IL scalar kind to size.
/// @return Required byte count in the bytecode global-storage area.
uint32_t bytecodeGlobalSize(il::core::Type::Kind kind) {
    using Kind = il::core::Type::Kind;
    switch (kind) {
        case Kind::I1:
            return 1;
        case Kind::I16:
            return 2;
        case Kind::I32:
            return 4;
        case Kind::I64:
        case Kind::F64:
        case Kind::Ptr:
        case Kind::Str:
        case Kind::Error:
        case Kind::ResumeTok:
            return 8;
        case Kind::Void:
            return 0;
    }
    return 8;
}

/// @brief Natural alignment (bytes) for a global of the given IL type kind;
///        I1→1, I16→2, I32→4, everything else 8.
/// @param kind IL scalar kind whose global alignment is required.
/// @return Power-of-two alignment in bytes.
uint32_t bytecodeGlobalAlign(il::core::Type::Kind kind) {
    using Kind = il::core::Type::Kind;
    switch (kind) {
        case Kind::I1:
            return 1;
        case Kind::I16:
            return 2;
        case Kind::I32:
            return 4;
        default:
            return 8;
    }
}

/// @brief Serialize @p value into @p dst as its raw little-endian-on-host
///        byte image (sizes @p dst to sizeof(T) first). Used to bake a
///        scalar global's compile-time initializer into the module.
/// @tparam T Trivially copyable scalar initializer type.
/// @param dst Destination vector replaced with exactly `sizeof(T)` bytes.
/// @param value Scalar whose object representation is copied.
template <typename T> void storeGlobalInitializer(std::vector<uint8_t> &dst, T value) {
    dst.resize(sizeof(T));
    std::memcpy(dst.data(), &value, sizeof(T));
}

/// @brief Parse a global's textual integer initializer (auto-base: 0x/0/dec).
/// @param global Borrowed IL global whose complete `init` string is parsed.
/// @return Signed 64-bit parsed value.
/// @throws std::invalid_argument if the whole string is not consumed (trailing
///         junk), so a malformed initializer becomes a clean compile failure.
/// @throws std::out_of_range If the spelling is outside the `int64_t` range.
int64_t parseI64Initializer(const il::core::Global &global) {
    size_t consumed = 0;
    int64_t value = std::stoll(global.init, &consumed, 0);
    if (consumed != global.init.size())
        throw std::invalid_argument("trailing characters");
    return value;
}

/// @brief Parse a global's textual floating-point initializer.
/// @param global Borrowed IL global whose complete `init` string is parsed.
/// @return Parsed double-precision value.
/// @throws std::invalid_argument on trailing characters (see
///         @ref parseI64Initializer).
/// @throws std::out_of_range If the spelling is outside the range accepted by
///         `std::stod`.
double parseF64Initializer(const il::core::Global &global) {
    size_t consumed = 0;
    double value = std::stod(global.init, &consumed);
    if (consumed != global.init.size())
        throw std::invalid_argument("trailing characters");
    return value;
}

/// @brief Align an alloca byte count to the VM's 8-byte allocation granularity.
/// @param size Non-negative requested byte count.
/// @return Aligned size, saturated to uint32_t max on overflow.
uint32_t alignAllocaByteCount(uint64_t size) {
    constexpr uint64_t kMaxU32 = std::numeric_limits<uint32_t>::max();
    if (size > kMaxU32 - 7u)
        return std::numeric_limits<uint32_t>::max();
    return static_cast<uint32_t>((size + 7u) & ~uint64_t{7u});
}

/// @brief Compute a raw-offset target used by EH and SWITCH metadata.
/// @details Raw branch offsets are stored in a separate 32-bit word and are
///          relative to the offset word itself, matching the VM dispatch logic.
/// @param offsetWordPc PC of the raw offset word.
/// @param encodedOffset Raw signed offset stored in the code stream.
/// @return Absolute target PC as a 64-bit signed value for range validation.
int64_t rawOffsetTarget(uint32_t offsetWordPc, uint32_t encodedOffset) {
    return static_cast<int64_t>(offsetWordPc) + static_cast<int32_t>(encodedOffset);
}

struct ArrayFastOpcode {
    /// Dedicated bytecode operation to emit.
    BCOpcode opcode;
    /// Exact native-helper operand count.
    uint32_t expectedArgs;
    /// Whether the helper must produce an IL result.
    bool hasReturn;
};

/// @brief Map a runtime array-accessor callee name to its fast-path bytecode
///        opcode (operand count + whether it returns a value), or nullopt if
///        @p callee is not one of the recognized rt_arr_*_fast helpers.
/// @details Lets the compiler replace a generic CALL_NATIVE to a hot array
///          getter/setter with a single dedicated opcode the VM inlines.
/// @param callee Borrowed native helper name matched case-sensitively.
/// @return Fast-path descriptor for a recognized helper; otherwise
///         `std::nullopt`.
std::optional<ArrayFastOpcode> arrayFastOpcodeFor(std::string_view callee) {
    if (callee == "rt_arr_i32_get_fast")
        return ArrayFastOpcode{BCOpcode::ARR_I32_GET_FAST, 2, true};
    if (callee == "rt_arr_i32_set_fast")
        return ArrayFastOpcode{BCOpcode::ARR_I32_SET_FAST, 3, false};
    if (callee == "rt_arr_i64_get_fast")
        return ArrayFastOpcode{BCOpcode::ARR_I64_GET_FAST, 2, true};
    if (callee == "rt_arr_i64_set_fast")
        return ArrayFastOpcode{BCOpcode::ARR_I64_SET_FAST, 3, false};
    if (callee == "rt_arr_f64_get_fast")
        return ArrayFastOpcode{BCOpcode::ARR_F64_GET_FAST, 2, true};
    if (callee == "rt_arr_f64_set_fast")
        return ArrayFastOpcode{BCOpcode::ARR_F64_SET_FAST, 3, false};
    return std::nullopt;
}
} // namespace

/// @brief Compile an IL module to a bytecode module (throwing wrapper).
/// @details Convenience over @ref compileChecked; turns a failure diagnostic
///          into a std::runtime_error. Prefer compileChecked when the caller
///          wants to report the diagnostic instead of unwinding.
/// @param ilModule Borrowed IL module to verify and lower.
/// @return Owning compiled bytecode module.
/// @throws std::runtime_error When checked compilation returns a diagnostic.
BytecodeModule BytecodeCompiler::compile(const il::core::Module &ilModule) {
    auto result = compileChecked(ilModule);
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
    return std::move(result.value());
}

/// @brief Compile an IL module to bytecode, returning a diagnostic on failure.
/// @param ilModule       The IL module to lower.
/// @param sourceManager  Optional; supplies source file/line for diagnostics
///                        and emitted LINE opcodes.
/// @param assumeVerified When false, the IL is run through the Verifier first
///                        and any failure is wrapped as a "bytecode preflight
///                        failed" diagnostic. Pass true only when the caller
///                        has already verified the module.
/// @details Function names are pre-registered before lowering so recursive and
///          forward calls resolve; globals are registered, then each function
///          is compiled. Compile failures unwind via BytecodeCompileFailure
///          and are returned as the Expected's error.
/// @return An owning bytecode module on success or one structured diagnostic on
///         verification, lowering, parsing, allocation, or internal failure.
/// @post Per-compilation state is reset for @p ilModule before preflight begins.
il::support::Expected<BytecodeModule> BytecodeCompiler::compileChecked(
    const il::core::Module &ilModule,
    const il::support::SourceManager *sourceManager,
    bool assumeVerified) {
    module_ = BytecodeModule();
    ilModule_ = &ilModule;
    currentFunc_ = nullptr;
    currentFunctionName_.clear();
    currentBlockLabel_.clear();
    currentLoc_ = {};
    sourceManager_ = sourceManager;
    sourceFileIndex_.clear();

    try {
        if (!assumeVerified) {
            if (auto verified = il::verify::Verifier::verify(ilModule); !verified) {
                auto diag = verified.error();
                if (diag.code.empty())
                    diag.code = "V-BC-IL-VERIFY";
                if (diag.message.find("bytecode preflight failed") == std::string::npos)
                    diag.message = "bytecode preflight failed: " + diag.message;
                return il::support::Expected<BytecodeModule>(std::move(diag));
            }
        }

        // Pre-register executable functions to support recursive and forward
        // calls. Import-linkage functions are declarations; calls to unresolved
        // imports lower through the native/runtime path instead of becoming
        // empty bytecode functions.
        uint32_t functionIndex = 0;
        for (const auto &fn : ilModule.functions) {
            if (fn.linkage == il::core::Linkage::Import)
                continue;
            if (functionIndex == std::numeric_limits<uint32_t>::max()) {
                fail({}, "V-BC-FUNCTION-TABLE", "bytecode supports at most 2^32 functions");
            }
            const std::string &name = fn.name;
            if (module_.functionIndex.find(name) != module_.functionIndex.end()) {
                fail({}, "V-BC-DUPLICATE-FUNCTION", "duplicate function @" + name);
            }
            module_.functionIndex[name] = functionIndex++;
        }

        registerGlobals(ilModule);

        // Compile each executable function. Import-linkage functions are
        // declarations with no body and are represented by native call entries
        // only if a call site references them.
        for (const auto &fn : ilModule.functions) {
            if (fn.linkage == il::core::Linkage::Import)
                continue;
            compileFunction(fn);
        }
    } catch (const BytecodeCompileFailure &failure) {
        return il::support::Expected<BytecodeModule>(failure.diag());
    } catch (const std::exception &ex) {
        return il::support::Expected<BytecodeModule>(il::support::Diag{
            il::support::Severity::Error,
            std::string("bytecode compile failed: internal compiler error: ") + ex.what(),
            {},
            "V-BC-INTERNAL"});
    } catch (...) {
        return il::support::Expected<BytecodeModule>(il::support::Diag{
            il::support::Severity::Error,
            "bytecode compile failed: internal compiler error",
            {},
            "V-BC-INTERNAL"});
    }

    return il::support::Expected<BytecodeModule>(std::move(module_));
}

/// @brief Assign every IL global a slot and bake its compile-time initializer.
/// @details Enforces the 65535-global limit and rejects duplicates and void
///          globals. String globals keep their text init; scalar globals get
///          their initializer parsed and stored as a raw byte image (an
///          invalid initializer is a clean V-BC-GLOBAL-INIT compile failure).
/// @param module Borrowed IL module whose globals are appended to `module_`.
/// @throws BytecodeCompileFailure On duplicate names, unsupported types, table
///         overflow, or malformed/out-of-range initializers.
void BytecodeCompiler::registerGlobals(const il::core::Module &module) {
    if (module.globals.size() > std::numeric_limits<uint16_t>::max()) {
        fail({}, "V-BC-GLOBAL-TABLE", "bytecode supports at most 65535 globals");
    }

    for (const auto &global : module.globals) {
        if (module_.globalIndex.find(global.name) != module_.globalIndex.end()) {
            fail({}, "V-BC-DUPLICATE-GLOBAL", "duplicate global @" + global.name);
        }

        if (global.type.kind == il::core::Type::Kind::Void) {
            fail({}, "V-BC-UNSUPPORTED-GLOBAL-TYPE", "void global @" + global.name);
        }

        GlobalInfo info;
        info.name = global.name;
        info.size = bytecodeGlobalSize(global.type.kind);
        info.align = bytecodeGlobalAlign(global.type.kind);
        info.type = global.type;

        if (global.type.kind == il::core::Type::Kind::Str) {
            info.initString = global.init;
        } else if (!global.init.empty()) {
            try {
                switch (global.type.kind) {
                    case il::core::Type::Kind::I1: {
                        const int64_t parsed = parseI64Initializer(global);
                        if (parsed != 0 && parsed != 1)
                            throw std::out_of_range("i1 initializer");
                        int8_t value = static_cast<int8_t>(parsed);
                        storeGlobalInitializer(info.initData, value);
                        break;
                    }
                    case il::core::Type::Kind::I16: {
                        const int64_t parsed = parseI64Initializer(global);
                        if (parsed < std::numeric_limits<int16_t>::min() ||
                            parsed > std::numeric_limits<int16_t>::max())
                            throw std::out_of_range("i16 initializer");
                        int16_t value = static_cast<int16_t>(parsed);
                        storeGlobalInitializer(info.initData, value);
                        break;
                    }
                    case il::core::Type::Kind::I32: {
                        const int64_t parsed = parseI64Initializer(global);
                        if (parsed < std::numeric_limits<int32_t>::min() ||
                            parsed > std::numeric_limits<int32_t>::max())
                            throw std::out_of_range("i32 initializer");
                        int32_t value = static_cast<int32_t>(parsed);
                        storeGlobalInitializer(info.initData, value);
                        break;
                    }
                    case il::core::Type::Kind::I64:
                    case il::core::Type::Kind::Error:
                    case il::core::Type::Kind::ResumeTok: {
                        int64_t value = parseI64Initializer(global);
                        storeGlobalInitializer(info.initData, value);
                        break;
                    }
                    case il::core::Type::Kind::F64: {
                        double value = parseF64Initializer(global);
                        storeGlobalInitializer(info.initData, value);
                        break;
                    }
                    case il::core::Type::Kind::Ptr: {
                        uintptr_t value = 0;
                        if (global.init != "null") {
                            const int64_t parsed = parseI64Initializer(global);
                            if (parsed < 0 ||
                                static_cast<uint64_t>(parsed) >
                                    static_cast<uint64_t>(
                                        std::numeric_limits<uintptr_t>::max())) {
                                throw std::out_of_range("ptr initializer");
                            }
                            value = static_cast<uintptr_t>(parsed);
                        }
                        storeGlobalInitializer(info.initData, value);
                        break;
                    }
                    case il::core::Type::Kind::Str:
                    case il::core::Type::Kind::Void:
                        break;
                }
            } catch (const std::exception &) {
                fail({}, "V-BC-GLOBAL-INIT", "invalid initializer for global @" + global.name);
            }
        }

        module_.addGlobal(std::move(info));
    }
}

/// @brief Emit LOAD_GLOBAL_ADDR for global @p name and push its address.
/// @details Fails the compile (V-BC-UNKNOWN-GLOBAL) if the name is unknown, or
///          V-BC-GLOBAL-TABLE if its index exceeds the 16-bit operand width.
/// @param name Borrowed global symbol name without the IL `@` sigil.
/// @param loc Source location attached to lookup diagnostics.
/// @throws BytecodeCompileFailure When lookup or index validation fails.
/// @post One address value is accounted for on the operand stack.
void BytecodeCompiler::emitGlobalAddress(std::string_view name, il::support::SourceLoc loc) {
    auto it = module_.globalIndex.find(std::string(name));
    if (it == module_.globalIndex.end()) {
        fail(loc, "V-BC-UNKNOWN-GLOBAL", "unknown global @" + std::string(name));
    }
    if (it->second > std::numeric_limits<uint16_t>::max()) {
        fail(loc, "V-BC-GLOBAL-TABLE", "global index exceeds bytecode operand width");
    }

    emit16(BCOpcode::LOAD_GLOBAL_ADDR, static_cast<uint16_t>(it->second));
    pushStack();
}

/// @brief Lower one IL function to a BytecodeFunction and add it to the module.
/// @details Resets per-function state, pre-scans eh.push targets so real
///          handler blocks can be told apart from forwarding blocks, builds
///          the SSA→locals map, linearizes the CFG, compiles each block,
///          back-patches branch offsets, and records the max stack depth.
/// @param fn Borrowed executable IL function.
/// @throws BytecodeCompileFailure When any validation or encoding limit fails.
/// @post A completed owning `BytecodeFunction` is appended to `module_`, and
///       current-function diagnostic context is cleared.
void BytecodeCompiler::compileFunction(const il::core::Function &fn) {
    // Create new bytecode function
    BytecodeFunction bcFunc;
    bcFunc.name = fn.name;
    bcFunc.numParams = static_cast<uint32_t>(fn.params.size());
    bcFunc.hasReturn = fn.retType.kind != il::core::Type::Kind::Void;

    // Set as current function
    currentFunc_ = &bcFunc;
    currentFunctionName_ = fn.name;

    // Reset compilation state
    ssaToLocal_.clear();
    blockOffsets_.clear();
    pendingBranches_.clear();
    ehPushTargets_.clear();
    currentStackDepth_ = 0;
    maxStackDepth_ = 0;
    maxAllocaSize_ = 0;

    // Collect eh.push target labels so we can distinguish real handler blocks
    // (entered via dispatchTrap) from forwarding handler blocks (entered via CBr).
    for (const auto &block : fn.blocks) {
        for (const auto &instr : block.instructions) {
            if (instr.op == il::core::Opcode::EhPush && !instr.labels.empty()) {
                ehPushTargets_.insert(instr.labels[0]);
            }
        }
    }

    // Build SSA to locals mapping
    buildSSAToLocalsMap(fn);
    bcFunc.numLocals = nextLocal_;
    bcFunc.localIsString = localIsString_;

    // Linearize blocks
    auto blocks = linearizeBlocks(fn);

    // Compile each block
    for (const auto *block : blocks) {
        compileBlock(*block);
    }

    // Resolve branch offsets
    resolveBranches();
    rebuildDerivedMetadata();

    // Record max stack depth
    bcFunc.maxStack = static_cast<uint32_t>(maxStackDepth_);
    bcFunc.allocaSize = maxAllocaSize_;

    // Add function to module
    module_.addFunction(std::move(bcFunc));
    currentFunc_ = nullptr;
    currentFunctionName_.clear();
    currentBlockLabel_.clear();
}

/// @brief Assign every SSA value (params, block params, instruction results)
///        a flat local-slot index, and flag which locals hold strings.
/// @details Function parameters get slots 0..N first; entry-block params alias
///          those same slots; all other block params and instruction results
///          get fresh slots. The string flags drive STR_RETAIN/RELEASE so
///          string locals are reference-counted correctly.
/// @param fn Borrowed IL function whose complete SSA namespace is indexed.
/// @throws BytecodeCompileFailure On duplicate block labels.
/// @post `nextLocal_`, `ssaToLocal_`, `blockParamIds_`, and `localIsString_`
///       describe every function parameter, block parameter, and result.
void BytecodeCompiler::buildSSAToLocalsMap(const il::core::Function &fn) {
    nextLocal_ = 0;
    localIsString_.clear();
    std::unordered_set<std::string> blockLabels;

    auto markLocalType = [this](uint32_t local, const il::core::Type &type) {
        if (local >= localIsString_.size())
            localIsString_.resize(local + 1, 0);
        if (type.kind == il::core::Type::Kind::Str)
            localIsString_[local] = 1;
    };

    // Map parameters first (preserve order)
    for (const auto &param : fn.params) {
        const uint32_t local = nextLocal_++;
        ssaToLocal_[param.id] = local;
        markLocalType(local, param.type);
    }

    // Map block parameters and track them by block label
    blockParamIds_.clear();
    bool isEntryBlock = true;
    for (const auto &block : fn.blocks) {
        if (!blockLabels.insert(block.label).second) {
            fail({}, "V-BC-DUPLICATE-BLOCK", "duplicate block label ^" + block.label);
        }
        std::vector<uint32_t> paramIds;
        for (size_t i = 0; i < block.params.size(); ++i) {
            const auto &param = block.params[i];
            paramIds.push_back(param.id);
            if (ssaToLocal_.find(param.id) == ssaToLocal_.end()) {
                // Entry block parameters correspond to function parameters
                // They should share the same local slots
                if (isEntryBlock && i < fn.params.size()) {
                    const uint32_t local = static_cast<uint32_t>(i);
                    ssaToLocal_[param.id] = local;
                    markLocalType(local, param.type);
                } else {
                    const uint32_t local = nextLocal_++;
                    ssaToLocal_[param.id] = local;
                    markLocalType(local, param.type);
                }
            } else {
                markLocalType(ssaToLocal_.at(param.id), param.type);
            }
        }
        blockParamIds_[block.label] = std::move(paramIds);
        isEntryBlock = false;
    }

    // Map instruction results
    for (const auto &block : fn.blocks) {
        for (const auto &instr : block.instructions) {
            if (instr.result) {
                if (ssaToLocal_.find(*instr.result) == ssaToLocal_.end()) {
                    const uint32_t local = nextLocal_++;
                    ssaToLocal_[*instr.result] = local;
                    markLocalType(local, instr.type);
                } else {
                    markLocalType(ssaToLocal_.at(*instr.result), instr.type);
                }
            }
        }
    }

    if (localIsString_.size() < nextLocal_)
        localIsString_.resize(nextLocal_, 0);
}

/// @brief Order the function's basic blocks for sequential code emission.
/// @details Depth-first from the entry block following terminator successors
///          (reversed so the DFS visits them in source order), then appends
///          any unreached blocks — notably exception-handler blocks, which
///          are entered via eh.push/dispatchTrap rather than normal edges.
/// @param fn Borrowed IL function whose block addresses remain stable.
/// @return Every block pointer exactly once, beginning with DFS-reachable
///         blocks and followed by unreachable blocks in source order.
std::vector<const il::core::BasicBlock *> BytecodeCompiler::linearizeBlocks(
    const il::core::Function &fn) {
    // Simple depth-first ordering
    std::vector<const il::core::BasicBlock *> result;
    std::unordered_set<const il::core::BasicBlock *> visited;
    std::unordered_map<std::string, const il::core::BasicBlock *> labelToBlock;

    // Build label -> block mapping
    for (const auto &block : fn.blocks) {
        labelToBlock[block.label] = &block;
    }

    // DFS from entry block
    if (!fn.blocks.empty()) {
        std::vector<const il::core::BasicBlock *> worklist;
        worklist.push_back(&fn.blocks.front());

        while (!worklist.empty()) {
            const auto *block = worklist.back();
            worklist.pop_back();

            if (visited.count(block))
                continue;
            visited.insert(block);
            result.push_back(block);

            // Add successor blocks (in reverse order for DFS)
            if (!block->instructions.empty()) {
                const auto &terminator = block->instructions.back();
                for (auto it = terminator.labels.rbegin(); it != terminator.labels.rend(); ++it) {
                    auto found = labelToBlock.find(*it);
                    if (found != labelToBlock.end() && !visited.count(found->second)) {
                        worklist.push_back(found->second);
                    }
                }
            }
        }
    }

    // Include any unvisited blocks (e.g., exception handler blocks)
    // Handler blocks are not reached through normal control flow but are
    // referenced by eh.push instructions
    for (const auto &block : fn.blocks) {
        if (!visited.count(&block)) {
            result.push_back(&block);
        }
    }

    return result;
}

/// @brief Emit code for one basic block, recording its byte offset for
///        branch resolution.
/// @details A true handler block (first instr is eh.entry AND it is a direct
///          eh.push target) receives its params on the operand stack from
///          dispatchTrap and pops them LIFO into locals; ordinary blocks get
///          their params via normal branch-argument stores. Then each
///          instruction is compiled in order.
/// @param block Borrowed block belonging to the current function.
/// @throws BytecodeCompileFailure On malformed handler parameters or instruction
///         lowering failure.
/// @post The block label is registered at its first emitted PC and cleared from
///       current diagnostic context after the block is compiled.
void BytecodeCompiler::compileBlock(const il::core::BasicBlock &block) {
    // Record block offset
    blockOffsets_[block.label] = static_cast<uint32_t>(currentFunc_->code.size());
    currentBlockLabel_ = block.label;
    currentLoc_ =
        block.instructions.empty() ? il::support::SourceLoc{} : block.instructions.front().loc;

    // Handle block parameters - they receive values from branch arguments
    // The calling block will have already pushed the arguments

    // Check if this is a handler block that receives values from dispatchTrap.
    // Only blocks that are direct targets of eh.push receive stack-pushed values
    // from dispatchTrap. Other blocks with eh.entry (e.g., typed-catch forwarding
    // blocks) receive their values via normal branch arguments stored to locals.
    bool isHandlerBlock = false;
    if (!block.params.empty() && !block.instructions.empty()) {
        if (block.instructions[0].op == il::core::Opcode::EhEntry &&
            ehPushTargets_.count(block.label)) {
            isHandlerBlock = true;
        }
    }

    if (isHandlerBlock) {
        if (block.params.size() != 2) {
            fail(currentLoc_,
                 "V-BC-HANDLER-SIGNATURE",
                 "exception handler block ^" + block.label +
                     " must declare exactly two params (error, resume_token)");
        }
        // Handler blocks receive values on the stack (error, resume_token)
        // pushed by dispatchTrap. Store them in reverse order (LIFO).
        auto it = blockParamIds_.find(block.label);
        if (it != blockParamIds_.end()) {
            const auto &paramIds = it->second;
            pushStack(static_cast<int32_t>(paramIds.size()));
            // Pop in reverse order since stack is LIFO
            for (size_t i = paramIds.size(); i > 0; --i) {
                uint32_t local = ssaToLocal_.at(paramIds[i - 1]);
                emitStoreLocal(local);
                popStack(1);
            }
        }
    }

    // Compile each instruction
    for (const auto &instr : block.instructions) {
        compileInstr(instr);
    }
    currentBlockLabel_.clear();
}

/// @brief Dispatch one IL instruction to the matching category compiler
///        (arithmetic / comparison / conversion / bitwise / memory / call /
///        branch / return / …), updating the current source location first.
/// @param instr Borrowed verified IL instruction.
/// @throws BytecodeCompileFailure On malformed operands, unsupported opcodes,
///         table limits, or invalid lowering metadata.
void BytecodeCompiler::compileInstr(const il::core::Instr &instr) {
    using Opcode = il::core::Opcode;

    currentLoc_ = instr.loc;

    switch (instr.op) {
        // Arithmetic
        case Opcode::Add:
        case Opcode::Sub:
        case Opcode::Mul:
        case Opcode::SDiv:
        case Opcode::UDiv:
        case Opcode::SRem:
        case Opcode::URem:
        case Opcode::IAddOvf:
        case Opcode::ISubOvf:
        case Opcode::IMulOvf:
        case Opcode::SDivChk0:
        case Opcode::UDivChk0:
        case Opcode::SRemChk0:
        case Opcode::URemChk0:
            compileArithmetic(instr);
            break;

        // Float arithmetic
        case Opcode::FAdd:
        case Opcode::FSub:
        case Opcode::FMul:
        case Opcode::FDiv:
            compileArithmetic(instr);
            break;

        // Comparisons
        case Opcode::ICmpEq:
        case Opcode::ICmpNe:
        case Opcode::SCmpLT:
        case Opcode::SCmpLE:
        case Opcode::SCmpGT:
        case Opcode::SCmpGE:
        case Opcode::UCmpLT:
        case Opcode::UCmpLE:
        case Opcode::UCmpGT:
        case Opcode::UCmpGE:
        case Opcode::FCmpEQ:
        case Opcode::FCmpNE:
        case Opcode::FCmpLT:
        case Opcode::FCmpLE:
        case Opcode::FCmpGT:
        case Opcode::FCmpGE:
        case Opcode::FCmpOrd:
        case Opcode::FCmpUno:
            compileComparison(instr);
            break;

        // Conversions
        case Opcode::Sitofp:
        case Opcode::Fptosi:
        case Opcode::CastFpToSiRteChk:
        case Opcode::CastFpToUiRteChk:
        case Opcode::CastSiNarrowChk:
        case Opcode::CastUiNarrowChk:
        case Opcode::CastSiToFp:
        case Opcode::CastUiToFp:
        case Opcode::Zext1:
        case Opcode::Trunc1:
            compileConversion(instr);
            break;

        // Bitwise
        case Opcode::And:
        case Opcode::Or:
        case Opcode::Xor:
        case Opcode::Shl:
        case Opcode::LShr:
        case Opcode::AShr:
            compileBitwise(instr);
            break;

        // Memory
        case Opcode::Alloca:
        case Opcode::GEP:
        case Opcode::Load:
        case Opcode::Store:
        case Opcode::AddrOf:
        case Opcode::GAddr:
        case Opcode::ConstStr:
        case Opcode::ConstNull:
            compileMemory(instr);
            break;

        // Control flow
        case Opcode::Call:
        case Opcode::CallIndirect:
            compileCall(instr);
            break;

        case Opcode::Br:
        case Opcode::CBr:
        case Opcode::SwitchI32:
            compileBranch(instr);
            break;

        case Opcode::Ret:
            compileReturn(instr);
            break;

        // Bounds check
        case Opcode::IdxChk:
            // Push operands: index, lo, hi
            requireOperandCount(instr, 3);
            pushValue(instr.operands[0]);
            pushValue(instr.operands[1]);
            pushValue(instr.operands[2]);
            emit8(BCOpcode::IDX_CHK, detail::encodeArithmeticWidthArg(instr.type.kind));
            popStack(2); // Consumes 3, produces 1
            storeResult(instr);
            break;

        // Value selection
        case Opcode::Select:
            // Push operands: cond, true value, false value
            requireOperandCount(instr, 3);
            pushValue(instr.operands[0]);
            pushValue(instr.operands[1]);
            pushValue(instr.operands[2]);
            emit(BCOpcode::SELECT);
            popStack(2); // Consumes 3, produces 1
            storeResult(instr);
            break;

        // Exception handling
        case Opcode::EhPush:
            // Push exception handler - emit opcode with placeholder for handler PC
            emit(BCOpcode::EH_PUSH);
            if (!instr.labels.empty()) {
                // Record fixup for handler label
                uint32_t offsetPos = static_cast<uint32_t>(currentFunc_->code.size());
                emit(0u); // Placeholder for handler PC
                // isRaw=true because the offset is in a separate word, not encoded in the
                // instruction
                pendingBranches_.push_back({offsetPos, instr.labels[0], false, true, instr.loc});
            }
            break;

        case Opcode::EhPop:
            emit(BCOpcode::EH_POP);
            break;

        case Opcode::EhEntry:
            // Handler entry marker - no-op but we need to set up handler params
            // The error and resume token will be on the stack when we enter
            emit(BCOpcode::EH_ENTRY);
            break;

        case Opcode::ErrGetKind:
            // Get trap kind from error object (on stack or in local)
            if (!instr.operands.empty()) {
                pushValue(instr.operands[0]);
            }
            emit(BCOpcode::ERR_GET_KIND);
            if (instr.operands.empty()) {
                pushStack(); // Result pushed
            }
            // else: consumed input, pushed output (net 0)
            storeResult(instr);
            break;

        case Opcode::ErrGetCode:
            if (!instr.operands.empty()) {
                pushValue(instr.operands[0]);
            }
            emit(BCOpcode::ERR_GET_CODE);
            if (instr.operands.empty()) {
                pushStack();
            }
            storeResult(instr);
            break;

        case Opcode::ErrGetIp:
            if (!instr.operands.empty()) {
                pushValue(instr.operands[0]);
            }
            emit(BCOpcode::ERR_GET_IP);
            if (instr.operands.empty()) {
                pushStack();
            }
            storeResult(instr);
            break;

        case Opcode::ErrGetLine:
            if (!instr.operands.empty()) {
                pushValue(instr.operands[0]);
            }
            emit(BCOpcode::ERR_GET_LINE);
            if (instr.operands.empty()) {
                pushStack();
            }
            storeResult(instr);
            break;

        case Opcode::ErrGetMsg:
            // Extract the trap message string from an error value, mirroring the
            // tree-walking VM's ErrGetMsg. Consumes the error token, produces an
            // owned rt_string (see BytecodeVM ERR_GET_MSG execution).
            if (!instr.operands.empty()) {
                pushValue(instr.operands[0]);
            }
            emit(BCOpcode::ERR_GET_MSG);
            if (instr.operands.empty()) {
                pushStack();
            }
            storeResult(instr);
            break;

        case Opcode::ResumeSame:
            // Resume at the faulting instruction
            if (!instr.operands.empty()) {
                pushValue(instr.operands[0]); // Push resume token
            }
            emit(BCOpcode::RESUME_SAME);
            if (!instr.operands.empty()) {
                popStack();
            }
            break;

        case Opcode::ResumeNext:
            // Resume after the faulting instruction
            if (!instr.operands.empty()) {
                pushValue(instr.operands[0]);
            }
            emit(BCOpcode::RESUME_NEXT);
            if (!instr.operands.empty()) {
                popStack();
            }
            break;

        case Opcode::ResumeLabel:
            // Resume at a specific label
            if (!instr.operands.empty()) {
                pushValue(instr.operands[0]); // Push resume token
            }
            emit(BCOpcode::RESUME_LABEL);
            if (!instr.operands.empty()) {
                popStack();
            }
            // Emit branch to the label
            if (!instr.labels.empty()) {
                uint32_t offsetPos = static_cast<uint32_t>(currentFunc_->code.size());
                emit(0u); // Placeholder
                pendingBranches_.push_back({offsetPos, instr.labels[0], false, true, instr.loc});
            }
            break;

        case Opcode::TrapKind:
            // Push the current trap kind (stored by dispatchTrap, no stack operand)
            emit(BCOpcode::TRAP_KIND);
            pushStack();
            storeResult(instr);
            break;

        case Opcode::TrapFromErr:
            // Push legacy error code operand onto stack; the bytecode VM maps
            // that code to a structured TrapKind while preserving the code.
            requireOperandCount(instr, 1);
            pushValue(instr.operands[0]);
            emit(BCOpcode::TRAP_FROM_ERR);
            break;

        case Opcode::TrapErr:
            // Bytecode represents Error values as their legacy numeric code.
            // This keeps trap.err non-terminating and lets trap.from_err and
            // err.get_* consume the value without starting the VM's trap path.
            requireOperandCount(instr, 1);
            if (!instr.result)
                fail(instr.loc, "V-BC-MALFORMED-INSTR", "trap.err must produce an error value");
            pushValue(instr.operands[0]);
            storeResult(instr);
            break;

        case Opcode::Trap:
            // Match the standard VM: plain IL trap is a domain error.
            emit8(BCOpcode::TRAP, static_cast<uint8_t>(TrapKind::DomainError));
            break;

        default:
            fail(instr.loc,
                 "V-BC-UNSUPPORTED-OP",
                 std::string("bytecode backend does not support IL opcode '") +
                     il::core::toString(instr.op) + "'");
    }
}

/// @brief Abort the current compile by throwing BytecodeCompileFailure.
/// @details Prefixes the message with the current function name and packages
///          @p code / @p loc into an error Diag; never returns. Caught at the
///          compileChecked boundary and surfaced as the Expected error.
/// @param loc Source location attached to the diagnostic.
/// @param code Stable diagnostic code moved into the failure.
/// @param message Human-readable detail, prefixed with current-function context
///                when available.
/// @throws BytecodeCompileFailure Always.
void BytecodeCompiler::fail(il::support::SourceLoc loc,
                            std::string code,
                            std::string message) const {
    if (!currentFunctionName_.empty())
        message = "bytecode compile failed in @" + currentFunctionName_ + ": " + message;
    throw BytecodeCompileFailure(
        il::support::Diag{il::support::Severity::Error, std::move(message), loc, std::move(code)});
}

/// @brief @ref fail using the instruction currently being compiled as the
///        source location.
/// @param code Stable diagnostic code forwarded by move.
/// @param message Human-readable diagnostic forwarded by move.
/// @throws BytecodeCompileFailure Always.
void BytecodeCompiler::failCurrent(std::string code, std::string message) const {
    fail(currentLoc_, std::move(code), std::move(message));
}

/// @brief Fail (V-BC-MALFORMED-INSTR) unless @p instr has at least @p minCount
///        operands — a guard against malformed IL reaching a category compiler.
/// @param instr Borrowed instruction whose operand vector is checked.
/// @param minCount Required minimum number of operands.
/// @throws BytecodeCompileFailure When the vector is too short.
void BytecodeCompiler::requireOperandCount(const il::core::Instr &instr, size_t minCount) const {
    if (instr.operands.size() < minCount) {
        std::ostringstream oss;
        oss << "opcode '" << il::core::toString(instr.op) << "' requires at least " << minCount
            << " operand";
        if (minCount != 1)
            oss << 's';
        oss << ", got " << instr.operands.size();
        fail(instr.loc, "V-BC-MALFORMED-INSTR", oss.str());
    }
}

/// @brief Emit code that pushes one IL operand value onto the operand stack.
/// @details Temps load from their local slot; small int constants use the
///          compact LOAD_ZERO/ONE/I8/I16 forms, larger ones go through the
///          constant pool; floats/strings go through the pool; a GlobalAddr
///          that names a function becomes a high-bit-tagged function pointer
///          (so CALL_INDIRECT can recognize it); null becomes LOAD_NULL.
/// @param val Borrowed IL value to materialize.
/// @throws BytecodeCompileFailure On unknown SSA/global references or pool and
///         local index overflow.
/// @post Exactly one value is accounted for on the operand stack.
void BytecodeCompiler::pushValue(const il::core::Value &val) {
    switch (val.kind) {
        case il::core::Value::Kind::Temp: {
            uint32_t local = getLocal(val.id);
            emitLoadLocal(local);
            pushStack();
            break;
        }

        case il::core::Value::Kind::ConstInt: {
            int64_t v = val.i64;
            if (v == 0) {
                emit(BCOpcode::LOAD_ZERO);
            } else if (v == 1) {
                emit(BCOpcode::LOAD_ONE);
            } else if (v >= -128 && v <= 127) {
                emitI8(BCOpcode::LOAD_I8, static_cast<int8_t>(v));
            } else if (v >= -32768 && v <= 32767) {
                emitI16(BCOpcode::LOAD_I16, static_cast<int16_t>(v));
            } else {
                // Add to constant pool
                uint32_t idx = module_.addI64(v);
                emitPoolLoad(BCOpcode::LOAD_I64, idx, "i64");
            }
            pushStack();
            break;
        }

        case il::core::Value::Kind::ConstFloat: {
            uint32_t idx = module_.addF64(val.f64);
            emitPoolLoad(BCOpcode::LOAD_F64, idx, "f64");
            pushStack();
            break;
        }

        case il::core::Value::Kind::ConstStr: {
            uint32_t idx = module_.addString(val.str);
            emitPoolLoad(BCOpcode::LOAD_STR, idx, "string");
            pushStack();
            break;
        }

        case il::core::Value::Kind::GlobalAddr: {
            // Check if this is a function reference
            auto it = module_.functionIndex.find(val.str);
            if (it != module_.functionIndex.end()) {
                // Function pointer - encode as tagged pointer (high bit set)
                // This allows call.indirect to identify function references
                uint64_t taggedPtr = 0x8000000000000000ULL | it->second;
                uint32_t idx = module_.addI64(static_cast<int64_t>(taggedPtr));
                emitPoolLoad(BCOpcode::LOAD_I64, idx, "i64");
                pushStack();
            } else {
                emitGlobalAddress(val.str, currentLoc_);
            }
            break;
        }

        case il::core::Value::Kind::NullPtr:
            emit(BCOpcode::LOAD_NULL);
            pushStack();
            break;
    }
}

/// @brief Pop the top of the operand stack into the instruction result's
///        local slot; a no-op for value-less instructions (e.g. stores).
/// @param instr Borrowed instruction whose optional result identifies the local.
/// @throws BytecodeCompileFailure When local lookup fails or stack accounting
///         would underflow.
void BytecodeCompiler::storeResult(const il::core::Instr &instr) {
    if (instr.result) {
        uint32_t local = getLocal(*instr.result);
        emitStoreLocal(local);
        popStack();
    } else {
        // No result - pop value if stack isn't empty
        // (some operations like stores don't produce values)
    }
}

/// @brief Intern @p loc's source file into the module's file table.
/// @return A 1-based table index (0 means "no file"), so callers can store
///         0 as a sentinel and subtract 1 to recover the real index. Paths
///         are resolved via the SourceManager, falling back to "file#<id>".
/// @param loc Source location whose file identifier is interned.
/// @post A previously unseen nonzero file identifier appends one module source
///       file entry and one cache mapping.
uint32_t BytecodeCompiler::sourceFileTableEntry(il::support::SourceLoc loc) {
    if (loc.file_id == 0)
        return 0;

    auto found = sourceFileIndex_.find(loc.file_id);
    if (found != sourceFileIndex_.end())
        return found->second + 1;

    std::string path;
    if (sourceManager_) {
        path = std::string(sourceManager_->getPath(loc.file_id));
    }
    if (path.empty()) {
        path = "file#" + std::to_string(loc.file_id);
    }

    const uint32_t index = static_cast<uint32_t>(module_.sourceFiles.size());
    module_.sourceFiles.push_back({std::move(path), 0});
    sourceFileIndex_.emplace(loc.file_id, index);
    return index + 1;
}

/// @brief Append one encoded 32-bit instruction word to the current function
///        and record its parallel debug metadata (line, source-file index,
///        block label). Latches the function's primary source file on the
///        first instruction that carries one.
/// @param instr Complete encoded instruction or raw inline metadata word.
/// @pre `currentFunc_` is non-null.
/// @post Code and all three parallel metadata tables grow by one element.
void BytecodeCompiler::emit(uint32_t instr) {
    const uint32_t pc = static_cast<uint32_t>(currentFunc_->code.size());
    currentFunc_->code.push_back(instr);
    currentFunc_->lineTable.push_back(currentLoc_.line);
    currentFunc_->sourceFileTable.push_back(sourceFileTableEntry(currentLoc_));
    currentFunc_->blockLabelTable.push_back(currentBlockLabel_);
    if (currentLoc_.file_id != 0 && currentFunc_->sourceFileIdx == 0) {
        const uint32_t tableEntry = currentFunc_->sourceFileTable[pc];
        if (tableEntry != 0)
            currentFunc_->sourceFileIdx = tableEntry - 1;
    }
}

/// @brief Emit an opcode with no inline operand.
/// @param op Bytecode operation to encode in the low eight bits.
void BytecodeCompiler::emit(BCOpcode op) {
    emit(encodeOp(op));
}

/// @brief Emit an opcode with an unsigned 8-bit inline operand.
/// @param op Bytecode operation to encode.
/// @param arg Unsigned payload stored in bits 8–15.
void BytecodeCompiler::emit8(BCOpcode op, uint8_t arg) {
    emit(encodeOp8(op, arg));
}

/// @brief Emit an opcode with a signed 8-bit inline operand.
/// @param op Bytecode operation to encode.
/// @param arg Signed payload stored by its eight-bit representation.
void BytecodeCompiler::emitI8(BCOpcode op, int8_t arg) {
    emit(encodeOpI8(op, arg));
}

/// @brief Emit an opcode with an unsigned 16-bit inline operand.
/// @param op Bytecode operation to encode.
/// @param arg Unsigned payload stored in bits 8–23.
void BytecodeCompiler::emit16(BCOpcode op, uint16_t arg) {
    emit(encodeOp16(op, arg));
}

/// @brief Emit an opcode with a signed 16-bit inline operand.
/// @param op Bytecode operation to encode.
/// @param arg Signed payload stored by its sixteen-bit representation.
void BytecodeCompiler::emitI16(BCOpcode op, int16_t arg) {
    emit(encodeOpI16(op, arg));
}

/// @brief Emit a constant-pool load (@p op with a 16-bit pool @p index),
///        failing the compile if the pool exceeds the 65535-entry operand
///        limit. @p poolName names the pool in the diagnostic.
/// @param op Pool-loading bytecode opcode.
/// @param index Zero-based constant-pool index.
/// @param poolName Borrowed human-readable pool kind for overflow diagnostics.
/// @throws BytecodeCompileFailure When @p index does not fit in 16 bits.
void BytecodeCompiler::emitPoolLoad(BCOpcode op, uint32_t index, std::string_view poolName) {
    if (index > 0xFFFFu) {
        failCurrent("V-BC-POOL-OVERFLOW",
                    "bytecode " + std::string(poolName) + " constant pool exceeds 65535 entries");
    }
    emit16(op, static_cast<uint16_t>(index));
}

/// @brief Emit an opcode with two packed unsigned 8-bit inline operands.
/// @param op Bytecode operation to encode.
/// @param arg0 Payload stored in bits 8–15.
/// @param arg1 Payload stored in bits 16–23.
// cppcheck-suppress unusedPrivateFunction
// cppcheck-suppress unusedFunction
void BytecodeCompiler::emit88(BCOpcode op, uint8_t arg0, uint8_t arg1) {
    emit(encodeOp88(op, arg0, arg1));
}

/// @brief Emit a short (16-bit relative) branch to @p label, recording a
///        pending fixup so @ref resolveBranches can back-patch the offset
///        once every block's byte offset is known.
/// @param op Short-branch opcode.
/// @param label Borrowed target label copied into pending-fixup storage.
/// @post One placeholder instruction and one fixup are appended.
void BytecodeCompiler::emitBranch(BCOpcode op, const std::string &label) {
    pendingBranches_.push_back({
        static_cast<uint32_t>(currentFunc_->code.size()),
        label,
        false, // isLong
        false, // isRaw
        currentLoc_
    });
    emit(encodeOp16(op, 0)); // Placeholder offset
}

/// @brief Emit a long (24-bit relative) branch to @p label for targets out
///        of 16-bit range, recording a pending fixup for @ref resolveBranches.
/// @param op Long-branch opcode.
/// @param label Borrowed target label copied into pending-fixup storage.
/// @post One placeholder instruction and one long fixup are appended.
void BytecodeCompiler::emitBranchLong(BCOpcode op, const std::string &label) {
    pendingBranches_.push_back({
        static_cast<uint32_t>(currentFunc_->code.size()),
        label,
        true, // isLong
        false, // isRaw
        currentLoc_
    });
    emit(encodeOp24(op, 0)); // Placeholder offset
}

/// @brief Back-patch every pending branch now that all block offsets are known.
/// @details Computes each target's relative offset (regular branches subtract
///          1 because dispatch advances pc past the instruction; raw-offset
///          words do not), re-encodes the placeholder in place as a 16- or
///          24-bit signed displacement, and fails on an unresolved label or a
///          displacement out of the opcode's range.
/// @throws BytecodeCompileFailure On missing labels, invalid placeholder PCs, or
///         displacements outside their selected encoding.
/// @post Every valid pending placeholder in current-function code contains its
///       finalized relative target.
void BytecodeCompiler::resolveBranches() {
    for (const auto &fixup : pendingBranches_) {
        auto it = blockOffsets_.find(fixup.targetLabel);
        if (it == blockOffsets_.end()) {
            fail(fixup.loc,
                 "V-BC-UNRESOLVED-BRANCH",
                 "unresolved bytecode branch target ^" + fixup.targetLabel);
        }
        if (fixup.codeOffset >= currentFunc_->code.size()) {
            fail(fixup.loc, "V-BC-BRANCH-FIXUP", "bytecode branch fixup offset out of range");
        }

        // For regular branches (opcode+offset encoded together), after DISPATCH
        // pc points past the instruction, so offset = target - (fixup_pos + 1)
        // For raw offsets (separate word), after reading the offset word,
        // pc - 1 points to the offset word, so: handlerPc = offset_pos + offset
        // Therefore for raw offsets: offset = target - offset_pos (no -1)
        int64_t offset =
            static_cast<int64_t>(it->second) - static_cast<int64_t>(fixup.codeOffset);
        if (!fixup.isRaw) {
            offset -= 1; // Regular branches need -1 adjustment
        }

        if (fixup.isRaw) {
            if (offset < std::numeric_limits<int32_t>::min() ||
                offset > std::numeric_limits<int32_t>::max()) {
                fail(fixup.loc,
                     "V-BC-BRANCH-RANGE",
                     "bytecode raw branch target is out of 32-bit range");
            }
            // Raw offset: store offset directly in the word
            currentFunc_->code[fixup.codeOffset] =
                static_cast<uint32_t>(static_cast<int32_t>(offset));
        } else {
            uint32_t instr = currentFunc_->code[fixup.codeOffset];
            BCOpcode op = decodeOpcode(instr);

            if (fixup.isLong) {
                if (offset < -0x800000 || offset > 0x7FFFFF) {
                    fail(fixup.loc,
                         "V-BC-BRANCH-RANGE",
                         "bytecode long branch target is out of 24-bit range");
                }
                currentFunc_->code[fixup.codeOffset] =
                    encodeOpI24(op, static_cast<int32_t>(offset));
            } else {
                if (offset < std::numeric_limits<int16_t>::min() ||
                    offset > std::numeric_limits<int16_t>::max()) {
                    fail(fixup.loc,
                         "V-BC-BRANCH-RANGE",
                         "bytecode branch target is out of 16-bit range");
                }
                currentFunc_->code[fixup.codeOffset] =
                    encodeOpI16(op, static_cast<int16_t>(offset));
            }
        }
    }
}

/// @brief Reconstruct derived exception and switch metadata from finalized code.
/// @details Branch resolution must run first because EH_PUSH and SWITCH store
///          raw relative offsets in their inline data. The scan validates that
///          those inline words exist and resolve inside the function before
///          publishing metadata to the BytecodeFunction.
/// @throws BytecodeCompileFailure On truncated inline metadata or an out-of-range
///         exception/switch target.
/// @post The current function's exception ranges and switch tables exactly
///       describe its finalized code stream.
void BytecodeCompiler::rebuildDerivedMetadata() {
    if (!currentFunc_)
        return;

    currentFunc_->exceptionRanges.clear();
    currentFunc_->switchTables.clear();

    struct PendingHandler {
        uint32_t startPc = 0;
        uint32_t handlerPc = 0;
    };
    std::vector<PendingHandler> handlerStack;

    const auto &code = currentFunc_->code;
    for (uint32_t pc = 0; pc < code.size(); ++pc) {
        const BCOpcode op = decodeOpcode(code[pc]);
        switch (op) {
            case BCOpcode::LOAD_I32:
                if (pc + 1 >= code.size())
                    failCurrent("V-BC-METADATA", "LOAD_I32 is missing its inline value word");
                ++pc;
                break;

            case BCOpcode::EH_PUSH: {
                if (pc + 1 >= code.size())
                    failCurrent("V-BC-METADATA", "EH_PUSH is missing its handler offset word");
                const uint32_t offsetPc = pc + 1;
                const int64_t target = rawOffsetTarget(offsetPc, code[offsetPc]);
                if (target < 0 || target >= static_cast<int64_t>(code.size())) {
                    failCurrent("V-BC-METADATA", "EH_PUSH handler target is out of range");
                }
                handlerStack.push_back(
                    PendingHandler{pc + 2, static_cast<uint32_t>(target)});
                pc = offsetPc;
                break;
            }

            case BCOpcode::EH_POP:
                if (!handlerStack.empty()) {
                    PendingHandler pending = handlerStack.back();
                    handlerStack.pop_back();
                    currentFunc_->exceptionRanges.push_back(
                        ExceptionRange{pending.startPc, pc, pending.handlerPc});
                }
                break;

            case BCOpcode::SWITCH: {
                if (pc + 2 >= code.size())
                    failCurrent("V-BC-METADATA", "SWITCH is missing its header words");
                const uint32_t numCases = code[pc + 1];
                const uint32_t defaultOffsetPc = pc + 2;
                const uint64_t caseWords = static_cast<uint64_t>(numCases) * 2u;
                const uint64_t endPc = static_cast<uint64_t>(pc) + 3u + caseWords;
                if (endPc > code.size()) {
                    failCurrent("V-BC-METADATA", "SWITCH case table extends past function code");
                }

                const int64_t defaultTarget = rawOffsetTarget(defaultOffsetPc, code[defaultOffsetPc]);
                if (defaultTarget < 0 || defaultTarget >= static_cast<int64_t>(code.size())) {
                    failCurrent("V-BC-METADATA", "SWITCH default target is out of range");
                }

                SwitchTable table;
                table.defaultPc = static_cast<uint32_t>(defaultTarget);
                uint32_t cursor = pc + 3;
                for (uint32_t i = 0; i < numCases; ++i) {
                    const int32_t caseValue = static_cast<int32_t>(code[cursor++]);
                    const uint32_t offsetPc = cursor++;
                    const int64_t target = rawOffsetTarget(offsetPc, code[offsetPc]);
                    if (target < 0 || target >= static_cast<int64_t>(code.size())) {
                        failCurrent("V-BC-METADATA", "SWITCH case target is out of range");
                    }
                    table.entries.push_back(
                        SwitchEntry{static_cast<int64_t>(caseValue), static_cast<uint32_t>(target)});
                }
                currentFunc_->switchTables.push_back(std::move(table));
                pc = static_cast<uint32_t>(endPc - 1u);
                break;
            }

            case BCOpcode::RESUME_LABEL:
                if (pc + 1 >= code.size())
                    failCurrent("V-BC-METADATA", "RESUME_LABEL is missing its target offset word");
                ++pc;
                break;

            default:
                break;
        }
    }

    for (const PendingHandler &pending : handlerStack) {
        currentFunc_->exceptionRanges.push_back(
            ExceptionRange{pending.startPc, static_cast<uint32_t>(code.size()), pending.handlerPc});
    }
}

/// @brief Track a function's statically known alloca use.
/// @details The VM rounds every alloca to 8 bytes and caps the alloca arena at
///          16 MiB. Constant alloca sizes are accumulated with saturation;
///          dynamic sizes mark the function as potentially using the full cap.
/// @param sizeOperand Borrowed IL alloca size value.
/// @post `maxAllocaSize_` remains monotonic and does not exceed 16 MiB unless
///       intermediate `uint32_t` saturation is recorded before the cap.
void BytecodeCompiler::recordAllocaSize(const il::core::Value &sizeOperand) {
    constexpr uint32_t kMaxAllocaBytes = 16u * 1024u * 1024u;
    if (sizeOperand.kind != il::core::Value::Kind::ConstInt) {
        maxAllocaSize_ = kMaxAllocaBytes;
        return;
    }
    if (sizeOperand.i64 < 0)
        return;

    const uint32_t aligned = alignAllocaByteCount(static_cast<uint64_t>(sizeOperand.i64));
    if (maxAllocaSize_ > std::numeric_limits<uint32_t>::max() - aligned) {
        maxAllocaSize_ = std::numeric_limits<uint32_t>::max();
        return;
    }
    maxAllocaSize_ = std::min<uint32_t>(kMaxAllocaBytes, maxAllocaSize_ + aligned);
}

/// @brief Account for @p count slots pushed onto the operand stack at compile
///        time, tracking the high-water mark used to size the VM frame.
/// @param count Number of logical slots added; callers normally pass a positive
///              value.
/// @post `currentStackDepth_` increases by @p count and `maxStackDepth_` is at
///       least the resulting current depth.
void BytecodeCompiler::pushStack(int32_t count) {
    currentStackDepth_ += count;
    if (currentStackDepth_ > maxStackDepth_) {
        maxStackDepth_ = currentStackDepth_;
    }
}

/// @brief Account for @p count slots popped from the operand stack at compile
///        time and fail if lowering attempts to underflow the stack.
/// @param count Non-negative number of logical slots consumed.
/// @throws BytecodeCompileFailure For a negative count or stack underflow.
void BytecodeCompiler::popStack(int32_t count) {
    if (count < 0) {
        failCurrent("V-BC-STACK-ACCOUNTING", "negative bytecode stack pop requested");
    }
    if (currentStackDepth_ < count) {
        failCurrent("V-BC-STACK-UNDERFLOW", "bytecode stack accounting underflow");
    }
    currentStackDepth_ -= count;
}

/// @brief Resolve an SSA value id to its assigned local slot; fails the
///        compile if unknown (IL verification should have rejected use-before-def).
/// @param ssaId Function-scoped SSA identifier.
/// @return Assigned zero-based local slot.
/// @throws BytecodeCompileFailure When the identifier was not indexed.
uint32_t BytecodeCompiler::getLocal(uint32_t ssaId) {
    auto it = ssaToLocal_.find(ssaId);
    if (it != ssaToLocal_.end()) {
        return it->second;
    }
    failCurrent("V-BC-UNKNOWN-SSA",
                "unknown SSA value %" + std::to_string(ssaId) +
                    " reached bytecode lowering; IL verification should reject use before def");
}

/// @brief Emit a load of local slot @p local, choosing the compact 8-bit
///        form for slots < 256 and the 16-bit wide form otherwise (fails
///        past the 65535-local limit).
/// @param local Zero-based local slot to encode.
/// @throws BytecodeCompileFailure When @p local exceeds 65535.
void BytecodeCompiler::emitLoadLocal(uint32_t local) {
    if (local < 256) {
        emit8(BCOpcode::LOAD_LOCAL, static_cast<uint8_t>(local));
    } else {
        if (local > 0xFFFFu) {
            failCurrent("V-BC-LOCAL-OVERFLOW", "bytecode supports at most 65535 locals");
        }
        emit16(BCOpcode::LOAD_LOCAL_W, static_cast<uint16_t>(local));
    }
}

/// @brief Emit a store to local slot @p local, choosing the compact 8-bit
///        form for slots < 256 and the 16-bit wide form otherwise (fails
///        past the 65535-local limit).
/// @param local Zero-based local slot to encode.
/// @throws BytecodeCompileFailure When @p local exceeds 65535.
void BytecodeCompiler::emitStoreLocal(uint32_t local) {
    if (local < 256) {
        emit8(BCOpcode::STORE_LOCAL, static_cast<uint8_t>(local));
    } else {
        if (local > 0xFFFFu) {
            failCurrent("V-BC-LOCAL-OVERFLOW", "bytecode supports at most 65535 locals");
        }
        emit16(BCOpcode::STORE_LOCAL_W, static_cast<uint16_t>(local));
    }
}

/// @brief Lower a binary integer/float arithmetic instruction: push both
///        operands, emit the matching ADD/SUB/MUL/DIV/REM/NEG bytecode
///        (including the .ovf / .chk checked variants), and store the result.
/// @param instr Borrowed arithmetic instruction with two operands and a result.
/// @throws BytecodeCompileFailure On insufficient operands, unsupported opcode,
///         missing locals, pool overflow, or stack-accounting failure.
/// @post The emitted operation has a net-zero logical stack effect after its
///       result is stored to a local.
void BytecodeCompiler::compileArithmetic(const il::core::Instr &instr) {
    using Opcode = il::core::Opcode;

    // Push operands
    requireOperandCount(instr, 2);
    pushValue(instr.operands[0]);
    pushValue(instr.operands[1]);

    // Emit operation
    BCOpcode bcOp;
    switch (instr.op) {
        case Opcode::Add:
            bcOp = BCOpcode::ADD_I64;
            break;
        case Opcode::Sub:
            bcOp = BCOpcode::SUB_I64;
            break;
        case Opcode::Mul:
            bcOp = BCOpcode::MUL_I64;
            break;
        case Opcode::SDiv:
            bcOp = BCOpcode::SDIV_I64;
            break;
        case Opcode::UDiv:
            bcOp = BCOpcode::UDIV_I64;
            break;
        case Opcode::SRem:
            bcOp = BCOpcode::SREM_I64;
            break;
        case Opcode::URem:
            bcOp = BCOpcode::UREM_I64;
            break;
        case Opcode::IAddOvf:
        case Opcode::ISubOvf:
        case Opcode::IMulOvf: {
            const uint8_t targetType = detail::encodeArithmeticWidthArg(instr.type.kind);
            BCOpcode op = (instr.op == Opcode::IAddOvf) ? BCOpcode::ADD_I64_OVF
                          : (instr.op == Opcode::ISubOvf) ? BCOpcode::SUB_I64_OVF
                                                          : BCOpcode::MUL_I64_OVF;
            emit8(op, targetType);
            popStack(); // Binary ops: consume 2, produce 1
            storeResult(instr);
            return; // Early return
        }
        case Opcode::SDivChk0:
        case Opcode::UDivChk0:
        case Opcode::SRemChk0:
        case Opcode::URemChk0: {
            BCOpcode op = (instr.op == Opcode::SDivChk0) ? BCOpcode::SDIV_I64_CHK
                          : (instr.op == Opcode::UDivChk0) ? BCOpcode::UDIV_I64_CHK
                          : (instr.op == Opcode::SRemChk0) ? BCOpcode::SREM_I64_CHK
                                                           : BCOpcode::UREM_I64_CHK;
            emit8(op, detail::encodeArithmeticWidthArg(instr.type.kind));
            popStack(); // Binary ops: consume 2, produce 1
            storeResult(instr);
            return;
        }
        case Opcode::FAdd:
            bcOp = BCOpcode::ADD_F64;
            break;
        case Opcode::FSub:
            bcOp = BCOpcode::SUB_F64;
            break;
        case Opcode::FMul:
            bcOp = BCOpcode::MUL_F64;
            break;
        case Opcode::FDiv:
            bcOp = BCOpcode::DIV_F64;
            break;
        default:
            fail(instr.loc,
                 "V-BC-UNSUPPORTED-OP",
                 std::string("bytecode backend does not support arithmetic opcode '") +
                     il::core::toString(instr.op) + "'");
    }

    emit(bcOp);
    popStack(); // Binary ops: consume 2, produce 1
    storeResult(instr);
}

/// @brief Lower a comparison instruction: push both operands and emit the
///        matching CMP_* opcode (signed/unsigned integer or float predicate),
///        leaving a 0/1 boolean result.
/// @param instr Borrowed comparison instruction with two operands and a result.
/// @throws BytecodeCompileFailure On insufficient operands, unsupported
///         predicates, operand materialization, or stack-accounting failure.
/// @post The boolean result is stored in its assigned local.
void BytecodeCompiler::compileComparison(const il::core::Instr &instr) {
    using Opcode = il::core::Opcode;

    // Push operands
    requireOperandCount(instr, 2);
    pushValue(instr.operands[0]);
    pushValue(instr.operands[1]);

    // Emit comparison
    BCOpcode bcOp;
    switch (instr.op) {
        case Opcode::ICmpEq:
            bcOp = BCOpcode::CMP_EQ_I64;
            break;
        case Opcode::ICmpNe:
            bcOp = BCOpcode::CMP_NE_I64;
            break;
        case Opcode::SCmpLT:
            bcOp = BCOpcode::CMP_SLT_I64;
            break;
        case Opcode::SCmpLE:
            bcOp = BCOpcode::CMP_SLE_I64;
            break;
        case Opcode::SCmpGT:
            bcOp = BCOpcode::CMP_SGT_I64;
            break;
        case Opcode::SCmpGE:
            bcOp = BCOpcode::CMP_SGE_I64;
            break;
        case Opcode::UCmpLT:
            bcOp = BCOpcode::CMP_ULT_I64;
            break;
        case Opcode::UCmpLE:
            bcOp = BCOpcode::CMP_ULE_I64;
            break;
        case Opcode::UCmpGT:
            bcOp = BCOpcode::CMP_UGT_I64;
            break;
        case Opcode::UCmpGE:
            bcOp = BCOpcode::CMP_UGE_I64;
            break;
        case Opcode::FCmpEQ:
            bcOp = BCOpcode::CMP_EQ_F64;
            break;
        case Opcode::FCmpNE:
            bcOp = BCOpcode::CMP_NE_F64;
            break;
        case Opcode::FCmpLT:
            bcOp = BCOpcode::CMP_LT_F64;
            break;
        case Opcode::FCmpLE:
            bcOp = BCOpcode::CMP_LE_F64;
            break;
        case Opcode::FCmpGT:
            bcOp = BCOpcode::CMP_GT_F64;
            break;
        case Opcode::FCmpGE:
            bcOp = BCOpcode::CMP_GE_F64;
            break;
        case Opcode::FCmpOrd:
            bcOp = BCOpcode::CMP_ORD_F64;
            break;
        case Opcode::FCmpUno:
            bcOp = BCOpcode::CMP_UNO_F64;
            break;
        default:
            fail(instr.loc,
                 "V-BC-UNSUPPORTED-OP",
                 std::string("bytecode backend does not support comparison opcode '") +
                     il::core::toString(instr.op) + "'");
    }

    emit(bcOp);
    popStack(); // Binary ops: consume 2, produce 1
    storeResult(instr);
}

/// @brief Lower a type-conversion instruction: push the source operand and
///        emit the matching widen/narrow/int↔float/bool opcode, including the
///        .chk variants that trap on range loss.
/// @param instr Borrowed conversion instruction with one operand and a result.
/// @throws BytecodeCompileFailure On insufficient operands, unsupported
///         conversion, operand materialization, or local lookup failure.
/// @post The converted value is stored in its assigned local.
void BytecodeCompiler::compileConversion(const il::core::Instr &instr) {
    using Opcode = il::core::Opcode;

    // Push operand
    requireOperandCount(instr, 1);
    pushValue(instr.operands[0]);

    // Emit conversion
    BCOpcode bcOp;
    switch (instr.op) {
        case Opcode::Sitofp:
        case Opcode::CastSiToFp:
            bcOp = BCOpcode::I64_TO_F64;
            break;
        case Opcode::CastUiToFp:
            bcOp = BCOpcode::U64_TO_F64;
            break;
        case Opcode::Fptosi:
            bcOp = BCOpcode::F64_TO_I64;
            break;
        case Opcode::CastFpToSiRteChk:
            emit8(BCOpcode::F64_TO_I64_CHK, detail::encodeArithmeticWidthArg(instr.type.kind));
            storeResult(instr);
            return;
        case Opcode::CastFpToUiRteChk:
            emit8(BCOpcode::F64_TO_U64_CHK, detail::encodeArithmeticWidthArg(instr.type.kind));
            storeResult(instr);
            return;
        case Opcode::CastSiNarrowChk:
        case Opcode::CastUiNarrowChk: {
            const uint8_t targetType = detail::encodeNarrowWidthArg(instr.type.kind);
            bcOp = (instr.op == Opcode::CastSiNarrowChk) ? BCOpcode::I64_NARROW_CHK
                                                         : BCOpcode::U64_NARROW_CHK;
            emit8(bcOp, targetType);
            storeResult(instr);
            return; // Early return - we've handled this case
        }
        case Opcode::Zext1:
            bcOp = BCOpcode::BOOL_TO_I64;
            break;
        case Opcode::Trunc1:
            bcOp = BCOpcode::I64_TO_BOOL;
            break;
        default:
            fail(instr.loc,
                 "V-BC-UNSUPPORTED-OP",
                 std::string("bytecode backend does not support conversion opcode '") +
                     il::core::toString(instr.op) + "'");
    }

    emit(bcOp);
    // Unary ops: consume 1, produce 1 - no stack change
    storeResult(instr);
}

/// @brief Lower a bitwise instruction: push both operands and emit the
///        matching AND/OR/XOR/NOT/SHL/LSHR/ASHR opcode.
/// @param instr Borrowed bitwise or shift instruction with two operands.
/// @throws BytecodeCompileFailure On insufficient operands, unsupported opcode,
///         operand materialization, or stack-accounting failure.
/// @post The emitted result is stored in its assigned local.
void BytecodeCompiler::compileBitwise(const il::core::Instr &instr) {
    using Opcode = il::core::Opcode;

    // Push operands
    requireOperandCount(instr, 2);
    pushValue(instr.operands[0]);
    pushValue(instr.operands[1]);

    // Emit operation
    BCOpcode bcOp;
    switch (instr.op) {
        case Opcode::And:
            bcOp = BCOpcode::AND_I64;
            break;
        case Opcode::Or:
            bcOp = BCOpcode::OR_I64;
            break;
        case Opcode::Xor:
            bcOp = BCOpcode::XOR_I64;
            break;
        case Opcode::Shl:
            bcOp = BCOpcode::SHL_I64;
            break;
        case Opcode::LShr:
            bcOp = BCOpcode::LSHR_I64;
            break;
        case Opcode::AShr:
            bcOp = BCOpcode::ASHR_I64;
            break;
        default:
            fail(instr.loc,
                 "V-BC-UNSUPPORTED-OP",
                 std::string("bytecode backend does not support bitwise opcode '") +
                     il::core::toString(instr.op) + "'");
    }

    emit(bcOp);
    popStack(); // Binary ops: consume 2, produce 1
    storeResult(instr);
}

/// @brief Lower memory/pointer instructions: null/alloca, GEP address
///        computation, and the typed load/store family, emitting the
///        width- and type-specific *_MEM opcode for each.
/// @param instr Borrowed memory, address, or constant instruction.
/// @throws BytecodeCompileFailure On malformed operands, unknown globals,
///         unsupported opcodes, pool limits, or stack/local accounting failure.
void BytecodeCompiler::compileMemory(const il::core::Instr &instr) {
    using Opcode = il::core::Opcode;

    switch (instr.op) {
        case Opcode::ConstNull:
            emit(BCOpcode::LOAD_NULL);
            pushStack();
            storeResult(instr);
            break;

        case Opcode::ConstStr:
            // String literal - operand can be ConstStr or GlobalAddr
            if (!instr.operands.empty()) {
                const auto &op = instr.operands[0];
                std::string strValue;
                bool found = false;

                if (op.kind == il::core::Value::Kind::ConstStr) {
                    // Direct string constant
                    strValue = op.str;
                    found = true;
                } else if (op.kind == il::core::Value::Kind::GlobalAddr) {
                    // Reference to a global string constant - look it up
                    if (ilModule_) {
                        auto it = std::find_if(ilModule_->globals.begin(),
                                               ilModule_->globals.end(),
                                               [&op](const il::core::Global &global) {
                                                   return global.name == op.str;
                                               });
                        if (it != ilModule_->globals.end()) {
                            strValue = it->init;
                            found = true;
                        }
                    }
                }

                if (found) {
                    uint32_t idx = module_.addString(strValue);
                    emitPoolLoad(BCOpcode::LOAD_STR, idx, "string");
                } else {
                    fail(instr.loc,
                         "V-BC-UNKNOWN-STRING-GLOBAL",
                         "const.str references unknown string global");
                }
            } else {
                fail(instr.loc, "V-BC-MALFORMED-INSTR", "const.str requires one operand");
            }
            pushStack();
            storeResult(instr);
            break;

        case Opcode::Alloca:
            requireOperandCount(instr, 1);
            recordAllocaSize(instr.operands[0]);
            pushValue(instr.operands[0]); // Size
            emit(BCOpcode::ALLOCA);
            // Alloca consumes 1, produces 1 - no stack change
            storeResult(instr);
            break;

        case Opcode::GEP:
            requireOperandCount(instr, 2);
            pushValue(instr.operands[0]); // Base pointer
            pushValue(instr.operands[1]); // Offset
            emit(BCOpcode::GEP);
            popStack(); // Consume 2, produce 1
            storeResult(instr);
            break;

        case Opcode::Load:
            // load type, ptr -> operands[0] is ptr (type is in instr.type)
            requireOperandCount(instr, 1);
            pushValue(instr.operands[0]);
            // Emit appropriate load based on type
            switch (instr.type.kind) {
                case il::core::Type::Kind::I1:
                    emit(BCOpcode::LOAD_I8_MEM);
                    break;
                case il::core::Type::Kind::I16:
                    emit(BCOpcode::LOAD_I16_MEM);
                    break;
                case il::core::Type::Kind::I32:
                    emit(BCOpcode::LOAD_I32_MEM);
                    break;
                case il::core::Type::Kind::F64:
                    emit(BCOpcode::LOAD_F64_MEM);
                    break;
                case il::core::Type::Kind::Ptr:
                    emit(BCOpcode::LOAD_PTR_MEM);
                    break;
                case il::core::Type::Kind::Str:
                    emit(BCOpcode::LOAD_STR_MEM);
                    break;
                default:
                    emit(BCOpcode::LOAD_I64_MEM);
                    break;
            }
            // Load consumes 1, produces 1 - no stack change
            storeResult(instr);
            break;

        case Opcode::Store:
            // store type, ptr, val -> operands[0] is ptr, operands[1] is val
            requireOperandCount(instr, 2);
            pushValue(instr.operands[0]); // Pointer
            pushValue(instr.operands[1]); // Value
            // Emit appropriate store based on type
            switch (instr.type.kind) {
                case il::core::Type::Kind::I1:
                    emit(BCOpcode::STORE_I8_MEM);
                    break;
                case il::core::Type::Kind::I16:
                    emit(BCOpcode::STORE_I16_MEM);
                    break;
                case il::core::Type::Kind::I32:
                    emit(BCOpcode::STORE_I32_MEM);
                    break;
                case il::core::Type::Kind::F64:
                    emit(BCOpcode::STORE_F64_MEM);
                    break;
                case il::core::Type::Kind::Ptr:
                    emit(BCOpcode::STORE_PTR_MEM);
                    break;
                case il::core::Type::Kind::Str:
                    emit(BCOpcode::STORE_STR_MEM);
                    break;
                default:
                    emit(BCOpcode::STORE_I64_MEM);
                    break;
            }
            popStack(2); // Consume 2, produce 0
            break;

        case Opcode::AddrOf:
            requireOperandCount(instr, 1);
            pushValue(instr.operands[0]);
            // AddrOf is identity for pointers in bytecode
            storeResult(instr);
            break;

        case Opcode::GAddr:
            requireOperandCount(instr, 1);
            if (instr.operands[0].kind != il::core::Value::Kind::GlobalAddr) {
                fail(instr.loc, "V-BC-MALFORMED-INSTR", "gaddr requires a global operand");
            }
            pushValue(instr.operands[0]);
            storeResult(instr);
            break;

        default:
            fail(instr.loc,
                 "V-BC-UNSUPPORTED-OP",
                 std::string("bytecode backend does not support memory opcode '") +
                     il::core::toString(instr.op) + "'");
    }
}

/// @brief Lower a call instruction.
/// @details Indirect calls push the callee function pointer plus arguments
///          and emit CALL_INDIRECT; direct calls resolve the callee to a
///          module function index or a native runtime helper (recognizing
///          rt_arr_*_fast accessors and substituting their dedicated fast
///          opcode), pushing the result when the callee returns a value.
/// @param instr Borrowed direct or indirect call instruction.
/// @throws BytecodeCompileFailure On malformed callees, argument/result
///         mismatches, table limits, or operand/stack accounting failure.
/// @post Arguments and any indirect callee are consumed; a returned value is
///       stored in the instruction's result local when present.
void BytecodeCompiler::compileCall(const il::core::Instr &instr) {
    using Opcode = il::core::Opcode;

    // Handle indirect calls separately
    if (instr.op == Opcode::CallIndirect) {
        // For call.indirect: operands[0] is the callee (function pointer)
        // operands[1..n] are the arguments
        if (instr.operands.empty()) {
            fail(instr.loc,
                 "V-BC-MALFORMED-INSTR",
                 "call.indirect requires a callee operand followed by arguments");
        }

        // Push callee (function pointer) first
        pushValue(instr.operands[0]);

        // Push arguments (operands[1..])
        for (size_t i = 1; i < instr.operands.size(); ++i) {
            pushValue(instr.operands[i]);
        }

        // Emit CALL_INDIRECT with argument count (not including callee)
        const size_t indirectArgCount = instr.operands.size() - 1;
        if (indirectArgCount > 0xFFu) {
            fail(instr.loc, "V-BC-CALL-ARGS", "CALL_INDIRECT supports at most 255 arguments");
        }
        uint8_t argCount = static_cast<uint8_t>(indirectArgCount);
        emit8(BCOpcode::CALL_INDIRECT, argCount);

        // Pop callee + arguments, push result if any
        popStack(static_cast<int32_t>(instr.operands.size()));
        if (instr.result) {
            pushStack();
            storeResult(instr);
        }
        return;
    }

    if (auto fast = arrayFastOpcodeFor(instr.callee)) {
        if (instr.operands.size() != fast->expectedArgs) {
            fail(instr.loc,
                 "V-BC-FAST-ARRAY-ARGS",
                 "array fast-path helper has unexpected argument count");
        }
        if (instr.result.has_value() != fast->hasReturn) {
            fail(instr.loc,
                 "V-BC-FAST-ARRAY-RESULT",
                 "array fast-path helper result does not match bytecode opcode");
        }

        for (const auto &arg : instr.operands)
            pushValue(arg);

        emit(fast->opcode);
        if (fast->hasReturn) {
            popStack(static_cast<int32_t>(fast->expectedArgs) - 1);
            storeResult(instr);
        } else {
            popStack(static_cast<int32_t>(fast->expectedArgs));
        }
        return;
    }

    // Regular direct call - push all arguments
    for (const auto &arg : instr.operands) {
        pushValue(arg);
    }

    // Look up function index
    auto it = module_.functionIndex.find(instr.callee);
    if (it != module_.functionIndex.end()) {
        if (it->second > 0xFFFFu) {
            fail(instr.loc, "V-BC-FUNCTION-TABLE", "CALL supports at most 65535 functions");
        }
        emit16(BCOpcode::CALL, static_cast<uint16_t>(it->second));
    } else {
        // External/native call
        const size_t argCount = instr.operands.size();
        if (argCount > 0xFFu) {
            fail(instr.loc, "V-BC-CALL-ARGS", "CALL_NATIVE supports at most 255 arguments");
        }

        uint32_t nativeIdx = module_.addNativeFunc(
            instr.callee, static_cast<uint32_t>(argCount), instr.result.has_value());
        if (nativeIdx > 0xFFFFu) {
            fail(instr.loc,
                 "V-BC-NATIVE-TABLE",
                 "CALL_NATIVE supports at most 65535 native references");
        }

        emit(encodeOp8_16(BCOpcode::CALL_NATIVE,
                          static_cast<uint8_t>(argCount),
                          static_cast<uint16_t>(nativeIdx)));
    }

    // Pop arguments, push result if any
    popStack(static_cast<int32_t>(instr.operands.size()));
    if (instr.result) {
        pushStack();
        storeResult(instr);
    }
}

/// @brief Lower an (un)conditional branch: store branch arguments into the
///        target block's parameter locals, then emit JUMP / JUMP_IF_* (or
///        SWITCH), recording fixups so @ref resolveBranches patches offsets.
/// @param instr Borrowed `br`, `cbr`, or `switch.i32` instruction.
/// @throws BytecodeCompileFailure On malformed labels/arguments/cases,
///         duplicate cases, synthetic-label exhaustion, or encoding limits.
/// @post All target setup paths and branch placeholders are emitted with no
///       net logical operand-stack growth.
void BytecodeCompiler::compileBranch(const il::core::Instr &instr) {
    using Opcode = il::core::Opcode;

    static const std::vector<il::core::Value> kNoBranchArgs;

    auto branchArgsAt = [&instr](size_t idx) -> const std::vector<il::core::Value> & {
        if (idx < instr.brArgs.size())
            return instr.brArgs[idx];
        return kNoBranchArgs;
    };

    // Helper to emit stores for branch arguments to block parameter locals
    auto storeBranchArgs = [this](const std::string &label,
                                  const std::vector<il::core::Value> &args) {
        auto it = blockParamIds_.find(label);
        if (it == blockParamIds_.end()) {
            if (!args.empty()) {
                failCurrent("V-BC-UNKNOWN-BRANCH-TARGET",
                            "branch arguments target unknown block ^" + label);
            }
            return;
        }
        const auto &paramIds = it->second;
        if (args.size() != paramIds.size()) {
            failCurrent("V-BC-BRANCH-ARGS",
                        "branch to ^" + label + " passes " + std::to_string(args.size()) +
                            " argument(s) for " + std::to_string(paramIds.size()) +
                            " block parameter(s)");
        }

        // Branch arguments are phi-edge values. Evaluate all sources before
        // assigning any target parameter so backedges can swap/reuse params
        // without clobbering a later source read.
        for (size_t i = 0; i < args.size(); ++i) {
            pushValue(args[i]);
        }
        for (size_t i = args.size(); i > 0; --i) {
            uint32_t local = getLocal(paramIds[i - 1]);
            emitStoreLocal(local);
            popStack();
        }
    };

    auto targetNeedsSetup = [this](const std::string &label,
                                   const std::vector<il::core::Value> &args) {
        auto it = blockParamIds_.find(label);
        if (it == blockParamIds_.end())
            return !args.empty();
        return !it->second.empty() || !args.empty();
    };
    std::unordered_set<std::string> syntheticLabels;
    auto makeSyntheticLabel = [this, &syntheticLabels](std::string_view prefix) {
        for (uint32_t attempt = 0; attempt < 1024; ++attempt) {
            std::string label = std::string(prefix) + std::to_string(currentFunc_->code.size()) +
                                "_" + std::to_string(attempt);
            if (blockOffsets_.find(label) == blockOffsets_.end() &&
                blockParamIds_.find(label) == blockParamIds_.end() &&
                syntheticLabels.find(label) == syntheticLabels.end()) {
                syntheticLabels.insert(label);
                return label;
            }
        }
        failCurrent("V-BC-SYNTHETIC-LABEL", "could not allocate unique bytecode setup label");
    };

    switch (instr.op) {
        case Opcode::Br: {
            if (instr.labels.empty()) {
                fail(instr.loc, "V-BC-MALFORMED-INSTR", "br requires a target label");
            }
            if (instr.brArgs.size() > 1) {
                fail(instr.loc, "V-BC-BRANCH-ARGS", "br has too many branch argument lists");
            }
            // Unconditional branch
            // Store branch arguments to target block's parameter locals
            storeBranchArgs(instr.labels[0], branchArgsAt(0));
            emitBranchLong(BCOpcode::JUMP_LONG, instr.labels[0]);
            break;
        }

        case Opcode::CBr: {
            requireOperandCount(instr, 1);
            if (instr.labels.size() < 2) {
                fail(instr.loc, "V-BC-MALFORMED-INSTR", "cbr requires true and false labels");
            }
            if (!instr.brArgs.empty() && instr.brArgs.size() != 2) {
                fail(instr.loc, "V-BC-BRANCH-ARGS", "cbr branch argument lists must match labels");
            }
            // Conditional branch: cbr %cond, thenLabel(args), elseLabel(args)
            pushValue(instr.operands[0]); // Condition

            // Keep the conditional branch itself short by targeting generated
            // setup code, then use long unconditional jumps to real blocks.
            std::string elseArgsLabel = makeSyntheticLabel("__else_args_");

            emitBranch(BCOpcode::JUMP_IF_FALSE, elseArgsLabel);
            popStack();

            storeBranchArgs(instr.labels[0], branchArgsAt(0));
            emitBranchLong(BCOpcode::JUMP_LONG, instr.labels[0]);

            blockOffsets_[elseArgsLabel] = static_cast<uint32_t>(currentFunc_->code.size());

            storeBranchArgs(instr.labels[1], branchArgsAt(1));
            emitBranchLong(BCOpcode::JUMP_LONG, instr.labels[1]);
            break;
        }

        case Opcode::SwitchI32: {
            requireOperandCount(instr, 1);
            if (instr.labels.empty()) {
                fail(instr.loc, "V-BC-MALFORMED-INSTR", "switch.i32 requires a default label");
            }
            if (!instr.brArgs.empty() && instr.brArgs.size() != instr.labels.size()) {
                fail(instr.loc,
                     "V-BC-BRANCH-ARGS",
                     "switch.i32 branch argument lists must match labels");
            }
            // Switch statement
            // Push the scrutinee value onto the stack
            pushValue(il::core::switchScrutinee(instr));

            const size_t numCases = il::core::switchCaseCount(instr);
            if (numCases > std::numeric_limits<uint32_t>::max()) {
                fail(instr.loc, "V-BC-SWITCH-CASE", "switch.i32 has too many cases");
            }
            if (instr.operands.size() != numCases + 1) {
                fail(instr.loc, "V-BC-MALFORMED-INSTR", "switch.i32 operands mismatch cases");
            }
            const std::string &defaultLabel = il::core::switchDefaultLabel(instr);

            // Emit SWITCH opcode
            emit(BCOpcode::SWITCH);
            popStack();

            // Emit number of cases (raw 32-bit word)
            emit(static_cast<uint32_t>(numCases));

            // Remember position for default offset and emit placeholder
            uint32_t defaultOffsetPos = static_cast<uint32_t>(currentFunc_->code.size());
            emit(0u); // placeholder for default offset

            struct SwitchTarget {
                std::string realLabel;
                std::string tableLabel;
                const std::vector<il::core::Value> *args = nullptr;
                bool needsSetup = false;
            };

            std::vector<SwitchTarget> switchTargets;
            switchTargets.reserve(numCases + 1);
            auto makeSwitchTarget = [&](size_t index, const std::string &realLabel) {
                const auto &args = branchArgsAt(index);
                SwitchTarget target;
                target.realLabel = realLabel;
                target.args = &args;
                target.needsSetup = targetNeedsSetup(realLabel, args);
                target.tableLabel =
                    target.needsSetup ? makeSyntheticLabel("__switch_args_" + std::to_string(index) +
                                                           "_")
                                      : realLabel;
                return target;
            };

            switchTargets.push_back(makeSwitchTarget(0, defaultLabel));

            // Remember positions for case offsets
            std::vector<std::pair<int32_t, uint32_t>> casePositions; // (caseValue, offsetPos)
            casePositions.reserve(numCases);
            std::unordered_set<int32_t> seenCases;
            for (size_t i = 0; i < numCases; ++i) {
                const auto &caseVal = il::core::switchCaseValue(instr, i);
                if (caseVal.kind != il::core::Value::Kind::ConstInt) {
                    fail(instr.loc, "V-BC-SWITCH-CASE", "switch.i32 case must be const int");
                }
                if (caseVal.i64 < std::numeric_limits<int32_t>::min() ||
                    caseVal.i64 > std::numeric_limits<int32_t>::max()) {
                    fail(instr.loc, "V-BC-SWITCH-CASE", "switch.i32 case out of i32 range");
                }
                int32_t caseInt = static_cast<int32_t>(caseVal.i64);
                if (!seenCases.insert(caseInt).second) {
                    fail(instr.loc, "V-BC-SWITCH-CASE", "switch.i32 has duplicate case value");
                }
                emit(static_cast<uint32_t>(caseInt)); // case value
                casePositions.push_back(
                    {caseInt, static_cast<uint32_t>(currentFunc_->code.size())});
                emit(0u); // placeholder for target offset
                switchTargets.push_back(makeSwitchTarget(i + 1, il::core::switchCaseLabel(instr, i)));
            }

            // Mark these as branch targets for later patching (using raw offsets)
            pendingBranches_.push_back(
                {defaultOffsetPos, switchTargets[0].tableLabel, false, true, instr.loc});

            for (size_t i = 0; i < numCases; ++i) {
                pendingBranches_.push_back({casePositions[i].second,
                                            switchTargets[i + 1].tableLabel,
                                            false,
                                            true,
                                            instr.loc});
            }

            for (const SwitchTarget &target : switchTargets) {
                if (!target.needsSetup)
                    continue;
                blockOffsets_[target.tableLabel] =
                    static_cast<uint32_t>(currentFunc_->code.size());
                storeBranchArgs(target.realLabel, *target.args);
                emitBranchLong(BCOpcode::JUMP_LONG, target.realLabel);
            }
            break;
        }

        default:
            break;
    }
}

/// @brief Lower a return: push the value and emit RETURN, or emit
///        RETURN_VOID when the function returns nothing.
/// @param instr Borrowed return instruction.
/// @throws BytecodeCompileFailure On multiple operands, value/void signature
///         mismatch, operand materialization, or stack-accounting failure.
/// @post A return terminator is appended and the logical stack depth is
///       restored after any value operand is consumed.
void BytecodeCompiler::compileReturn(const il::core::Instr &instr) {
    if (instr.operands.size() > 1) {
        fail(instr.loc, "V-BC-RETURN", "return instruction has too many operands");
    }
    if (!instr.operands.empty()) {
        if (!currentFunc_ || !currentFunc_->hasReturn) {
            fail(instr.loc, "V-BC-RETURN", "void function cannot return a value");
        }
        pushValue(instr.operands[0]);
        emit(BCOpcode::RETURN);
        popStack();
    } else {
        if (currentFunc_ && currentFunc_->hasReturn) {
            fail(instr.loc, "V-BC-RETURN", "non-void function must return a value");
        }
        emit(BCOpcode::RETURN_VOID);
    }
}

} // namespace bytecode
} // namespace zanna
