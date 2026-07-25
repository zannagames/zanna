//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/build/IRBuilder.cpp
// Purpose: Provide a structured API for constructing IL modules programmatically.
// Key invariants: Builder maintains a current function/block insertion context
//                 and monotonically increasing SSA identifiers.
// Ownership/Lifetime: Builder references a module owned by the caller.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the fluent builder used by front ends to emit IL.
/// @details The IRBuilder records insertion points, tracks SSA identifiers, and
///          materialises instructions while enforcing control-flow invariants
///          such as single terminators per block.  It also caches callee return
///          types to validate call emission.

#include "il/build/IRBuilder.hpp"
#include "il/core/OpcodeInfo.hpp"
#include <algorithm>
#include <cassert>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace il::build {
using namespace il::core;
using namespace il::support;

//===----------------------------------------------------------------------===//
// Validation Helpers
//===----------------------------------------------------------------------===//

/// @brief Check that a builder-visible name is present.
/// @details Empty names produce malformed IL and can poison callee or block
///          lookup caches. Builder APIs treat them as programming errors in
///          every build mode.
/// @param value Name to validate.
/// @param role Human-readable role for the thrown diagnostic.
static void validateNonEmptyName(std::string_view value, const char *role) {
    if (value.empty())
        throw std::invalid_argument(std::string("IRBuilder: ") + role + " cannot be empty");
}

/// @brief Check that all parameters have non-void types.
/// @details Void parameters cannot be represented as SSA values. Enforcing this
///          before insertion keeps the function/block parameter lists valid even
///          when assertions are disabled.
/// @param params Parameters to validate.
static void validateParamTypes(const std::vector<Param> &params) {
    for (const auto &p : params) {
        if (p.type.kind == Type::Kind::Void)
            throw std::invalid_argument("IRBuilder: parameter cannot have Void type");
    }
}

/// @brief Check whether a module already declares a function named @p name.
/// @param module Module to inspect.
/// @param name Function name to find.
/// @return True when the name is already used by a function.
static bool moduleHasFunction(const Module &module, std::string_view name) {
    return std::any_of(module.functions.begin(), module.functions.end(), [&](const Function &fn) {
        return fn.name == name;
    });
}

/// @brief Check whether a module already declares an extern named @p name.
/// @param module Module to inspect.
/// @param name Extern name to find.
/// @return True when the name is already used by an extern declaration.
static bool moduleHasExtern(const Module &module, std::string_view name) {
    return std::any_of(module.externs.begin(), module.externs.end(), [&](const Extern &ext) {
        return ext.name == name;
    });
}

/// @brief Check whether a function already contains a block label.
/// @param function Function to inspect.
/// @param label Candidate block label.
/// @return True when @p label is already present in @p function.
static bool functionHasBlockLabel(const Function &function, std::string_view label) {
    return std::any_of(function.blocks.begin(), function.blocks.end(), [&](const BasicBlock &bb) {
        return bb.label == label;
    });
}

/// @brief Validate that a value does not reference a future temporary.
/// @details Builder-created operands must reference parameters or instruction
///          results already reserved in the active function. Constants and
///          globals are always accepted.
/// @param value Operand to inspect.
/// @param nextTemp First not-yet-allocated temporary id.
/// @param context Description of the operand role.
static void validateTempValue(const Value &value, unsigned nextTemp, const char *context) {
    if (value.kind == Value::Kind::Temp && value.id >= nextTemp) {
        throw std::logic_error(std::string("IRBuilder: ") + context +
                               " temp ID exceeds allocated temporaries");
    }
}

/// @brief Validate every temporary operand in a value list.
/// @param values Operands to inspect.
/// @param nextTemp First not-yet-allocated temporary id.
/// @param context Description of the operand list role.
static void validateTempValues(const std::vector<Value> &values,
                               unsigned nextTemp,
                               const char *context) {
    for (const auto &value : values)
        validateTempValue(value, nextTemp, context);
}

/// @brief Record the largest temporary identifier referenced by @p value.
/// @details Ignores constants and globals because only SSA temporaries
///          participate in the builder's `nextTemp` collision checks.
/// @param value Operand to inspect.
/// @param nextRequired Updated to at least one greater than @p value's temp id.
static void noteLiveTemp(const Value &value, unsigned &nextRequired) {
    if (value.kind != Value::Kind::Temp)
        return;
    if (value.id == std::numeric_limits<unsigned>::max())
        throw std::overflow_error("IRBuilder: live temp ID overflows");
    nextRequired = std::max(nextRequired, value.id + 1);
}

/// @brief Compute the first safe temporary identifier for a function.
/// @details Scans parameters, block parameters, instruction results, operands,
///          and branch arguments so restoring the builder counter cannot reuse a
///          temporary that already appears in live IR.
/// @param fn Function whose live temporary namespace is being scanned.
/// @return One greater than the largest temporary id referenced by @p fn.
static unsigned firstUnusedTempId(const Function &fn) {
    unsigned nextRequired = 0;
    for (const auto &param : fn.params) {
        if (param.id == std::numeric_limits<unsigned>::max())
            throw std::overflow_error("IRBuilder: function parameter temp ID overflows");
        nextRequired = std::max(nextRequired, param.id + 1);
    }
    for (const auto &block : fn.blocks) {
        for (const auto &param : block.params) {
            if (param.id == std::numeric_limits<unsigned>::max())
                throw std::overflow_error("IRBuilder: block parameter temp ID overflows");
            nextRequired = std::max(nextRequired, param.id + 1);
        }
        for (const auto &instr : block.instructions) {
            if (instr.result) {
                if (*instr.result == std::numeric_limits<unsigned>::max())
                    throw std::overflow_error("IRBuilder: instruction result temp ID overflows");
                nextRequired = std::max(nextRequired, *instr.result + 1);
            }
            for (const auto &operand : instr.operands)
                noteLiveTemp(operand, nextRequired);
            for (const auto &argList : instr.brArgs)
                for (const auto &arg : argList)
                    noteLiveTemp(arg, nextRequired);
        }
    }
    return nextRequired;
}

/// @brief Validate edge arguments against a target block's parameter list.
/// @details Branch argument count mismatches make SSA block parameters
///          ill-formed. The builder reports them before appending terminators so
///          malformed blocks are not partially constructed.
/// @param target Destination block receiving the edge.
/// @param args Values supplied to @p target.
/// @param nextTemp First not-yet-allocated temporary id.
/// @param context Human-readable edge description.
static void validateBranchArgs(const BasicBlock &target,
                               const std::vector<Value> &args,
                               unsigned nextTemp,
                               const char *context) {
    if (args.size() != target.params.size()) {
        throw std::invalid_argument(std::string("IRBuilder: ") + context +
                                    " argument count must match target block parameters");
    }
    validateTempValues(args, nextTemp, context);
}

/// @brief Initialise a builder that mutates an existing module.
///
/// @details The constructor walks existing functions and extern declarations to
///          seed the @ref calleeReturnTypes cache.  This enables later calls to
///          @ref emitCall to validate that callees exist and to stamp the
///          expected result type before any new instructions are emitted.
///          In debug builds, also initializes hash sets for O(1) uniqueness checking.
///
/// @param m Module that will be extended by this builder.
/// @note Populates callee metadata so emitCall can validate future calls.
IRBuilder::IRBuilder(il::core::Module &m) : mod(m) {
    mod.internOwnedIdentifiers();
    for (const auto &fn : mod.functions) {
        calleeReturnTypes[fn.name] = fn.retType;
#ifndef NDEBUG
        usedFunctionNames_.insert(fn.name);
#endif
    }
    for (const auto &ex : mod.externs) {
        calleeReturnTypes[ex.name] = ex.retType;
#ifndef NDEBUG
        usedExternNames_.insert(ex.name);
#endif
    }
}

/// @brief Declare an external function and record its signature.
/// @param name Symbol name for the external function.
/// @param ret Return type advertised to callers.
/// @param params Ordered parameter types accepted by the callee.
/// @return Reference to the inserted extern declaration.
/// @post calleeReturnTypes is updated to match @p ret.
il::core::Extern &IRBuilder::addExtern(const std::string &name,
                                       il::core::Type ret,
                                       const std::vector<il::core::Type> &params) {
    validateNonEmptyName(name, "extern name");
    if (moduleHasExtern(mod, name) || moduleHasFunction(mod, name))
        throw std::invalid_argument("IRBuilder: extern name already exists in module");
#ifndef NDEBUG
    // Extern name must not be empty
    assert(!name.empty() && "extern name cannot be empty");
    // Extern name must be unique - O(1) hash set lookup instead of O(n) linear scan
    assert(usedExternNames_.find(name) == usedExternNames_.end() &&
           "extern name already exists in module");
    usedExternNames_.insert(name);
#endif

    mod.externs.push_back({name, ret, params});
    mod.externs.back().nameSymbol = mod.internIdentifier(name);
    calleeReturnTypes[name] = ret;
    return mod.externs.back();
}

/// @brief Add a global variable with specified type.
/// @param name Global identifier without leading `@`.
/// @param type Variable type.
/// @param init Optional initializer (empty for zero-initialized).
/// @return Reference to the inserted global definition.
il::core::Global &IRBuilder::addGlobal(const std::string &name,
                                       il::core::Type type,
                                       const std::string &init) {
    validateNonEmptyName(name, "global name");
#ifndef NDEBUG
    // Global name must not be empty
    assert(!name.empty() && "global name cannot be empty");
#endif

    mod.globals.push_back({name, type, init});
    mod.globals.back().hasInitializer = !init.empty();
    mod.globals.back().nameSymbol = mod.internIdentifier(name);
    return mod.globals.back();
}

/// @brief Add a UTF-8 string literal as a global value.
/// @param name Identifier to attach to the global string.
/// @param value Contents of the string literal.
/// @return Reference to the inserted global definition.
/// @note The global is always recorded with Type::Kind::Str.
il::core::Global &IRBuilder::addGlobalStr(const std::string &name, const std::string &value) {
    auto &global = addGlobal(name, il::core::Type(il::core::Type::Kind::Str), value);
    global.isConst = true;
    global.hasInitializer = true;
    return global;
}

/// @brief Begin building a new function and make it the active insertion target.
/// @param name Symbol name for the function being defined.
/// @param ret Return type to advertise to callers.
/// @param params Formal parameters with stable IDs and debug names.
/// @return Reference to the newly created function.
/// @post nextTemp is reset and populated with parameter IDs for subsequent temporaries.
il::core::Function &IRBuilder::startFunction(const std::string &name,
                                             il::core::Type ret,
                                             const std::vector<il::core::Param> &params) {
    validateNonEmptyName(name, "function name");
    validateParamTypes(params);
    if (moduleHasFunction(mod, name) || moduleHasExtern(mod, name))
        throw std::invalid_argument("IRBuilder: function name already exists in module");
#ifndef NDEBUG
    // Function name must not be empty
    assert(!name.empty() && "function name cannot be empty");
    // Validate parameter types (no Void parameters allowed)
    validateParamTypes(params);
    assert(usedFunctionNames_.find(name) == usedFunctionNames_.end() &&
           "function name already exists in module");
    usedFunctionNames_.insert(name);
    // Initialize per-function block label tracking for this function
    usedBlockLabelsPerFunc_[name].clear();
#endif

    il::core::Function fn;
    fn.name = name;
    fn.nameSymbol = mod.internIdentifier(name);
    fn.retType = ret;
    mod.functions.push_back(std::move(fn));
    calleeReturnTypes[name] = ret;
    curFunc = &mod.functions.back();
    curBlockIdx.reset();
    nextTemp = 0;
    for (auto p : params) {
        il::core::Param np = p;
        np.id = nextTemp++;
        curFunc->params.push_back(np);
    }
    curFunc->valueNames.resize(nextTemp);
    for (const auto &p : curFunc->params)
        curFunc->valueNames[p.id] = p.name;
    return *curFunc;
}

/// @brief Restore a saved temporary counter after validating the active namespace.
/// @details Frontends sometimes save the current counter while temporarily
///          switching contexts.  Restoring below a live temp would allow the next
///          emission to reuse an existing SSA identifier, so the function scans
///          the active function before committing the counter.
/// @param saved Previously saved first-available temporary identifier.
/// @throws std::logic_error when @p saved would collide with existing IR.
void IRBuilder::restoreTempId(unsigned saved) {
    if (curFunc) {
        const unsigned required = firstUnusedTempId(*curFunc);
        if (saved < required)
            throw std::logic_error("IRBuilder: restoreTempId would reuse a live temporary");
    }
    nextTemp = saved;
}

/// @brief Create a basic block in @p fn with optional block parameters.
/// @param fn Function that owns the block; typically the function returned by startFunction().
/// @param label Unique label used when referencing the block.
/// @param params Block parameters that must align with incoming branch arguments.
/// @return Reference to the created block.
/// @post fn.valueNames is grown to include every parameter ID produced.
il::core::BasicBlock &IRBuilder::createBlock(il::core::Function &fn,
                                             const std::string &label,
                                             const std::vector<il::core::Param> &params) {
    validateNonEmptyName(label, "block label");
    validateParamTypes(params);
    if (functionHasBlockLabel(fn, label))
        throw std::invalid_argument("IRBuilder: block label already exists in function");
#ifndef NDEBUG
    // Block label must not be empty
    assert(!label.empty() && "block label cannot be empty");
    // Block label must be unique within the function - O(1) hash set lookup instead of O(n) linear
    // scan
    auto &funcLabels = usedBlockLabelsPerFunc_[fn.name];
    assert(funcLabels.find(label) == funcLabels.end() && "block label already exists in function");
    funcLabels.insert(label);
    // Validate block parameter types (no Void parameters allowed)
    validateParamTypes(params);
#endif

    fn.blocks.push_back({label, {}, {}, false});
    il::core::BasicBlock &bb = fn.blocks.back();
    bb.labelSymbol = mod.internIdentifier(label);
    for (auto p : params) {
        il::core::Param np = p;
        np.id = nextTemp++;
        bb.params.push_back(np);
        if (fn.valueNames.size() <= np.id)
            fn.valueNames.resize(np.id + 1);
        fn.valueNames[np.id] = np.name;
    }
    return bb;
}

/// @brief Convenience wrapper for creating a block without parameters.
/// @param fn Function to receive the block.
/// @param label Label to assign to the block.
/// @return Reference to the created block.
il::core::BasicBlock &IRBuilder::addBlock(il::core::Function &fn, const std::string &label) {
    return createBlock(fn, label, {});
}

/// @brief Insert a parameter-less basic block at a fixed position in the function.
/// @details Useful for ensuring new blocks appear before a known position (e.g.,
///          the function's synthetic exit block). Does not update the current
///          insertion point.
/// @param fn Function receiving the block.
/// @param idx Requested zero-based insertion position; values past the end are clamped.
/// @param label Unique non-empty block label.
/// @return Reference to the inserted block.
il::core::BasicBlock &IRBuilder::insertBlock(il::core::Function &fn,
                                             size_t idx,
                                             const std::string &label) {
    validateNonEmptyName(label, "block label");
    if (functionHasBlockLabel(fn, label))
        throw std::invalid_argument("IRBuilder: block label already exists in function");
#ifndef NDEBUG
    // Block label must not be empty
    assert(!label.empty() && "block label cannot be empty");
    // Block label must be unique within the function - O(1) hash set lookup instead of O(n) linear
    // scan
    auto &funcLabels = usedBlockLabelsPerFunc_[fn.name];
    assert(funcLabels.find(label) == funcLabels.end() && "block label already exists in function");
    funcLabels.insert(label);
#endif

    if (idx > fn.blocks.size())
        idx = fn.blocks.size();
    il::core::BasicBlock block{label, {}, {}, false};
    block.labelSymbol = mod.internIdentifier(label);
    auto it = fn.blocks.insert(fn.blocks.begin() + static_cast<std::ptrdiff_t>(idx),
                               std::move(block));
    return *it;
}

/// @brief Retrieve the SSA value associated with a block parameter.
/// @param bb Block whose parameter is being referenced.
/// @param idx Zero-based index into bb.params.
/// @return Temporary value representing the parameter.
/// @pre idx must reference an existing parameter.
il::core::Value IRBuilder::blockParam(il::core::BasicBlock &bb, unsigned idx) {
    assert(idx < bb.params.size());
    if (idx >= bb.params.size())
        throw std::out_of_range("IRBuilder: block parameter index out of range");
    return il::core::Value::temp(bb.params[idx].id);
}

/// @brief Emit an unconditional branch to @p dst.
/// @param dst Destination block that receives control.
/// @param args Values corresponding to @p dst's block parameters.
/// @pre args.size() must equal dst.params.size().
/// @post Current block is marked terminated and cannot receive more non-phi instructions.
void IRBuilder::br(il::core::BasicBlock &dst, const std::vector<il::core::Value> &args) {
    assert(args.size() == dst.params.size() &&
           "branch argument count must match block parameter count");
    validateBranchArgs(dst, args, nextTemp, "branch");

#ifndef NDEBUG
    // Validate all branch argument temp IDs are within bounds
    for (const auto &arg : args) {
        if (arg.kind == Value::Kind::Temp) {
            assert(arg.id < nextTemp &&
                   "branch argument temp ID exceeds allocated temporaries (dangling reference)");
        }
    }
#endif

    il::core::Instr instr;
    instr.op = il::core::Opcode::Br;
    instr.type = il::core::Type(il::core::Type::Kind::Void);
    instr.addBranchTarget(mod, dst.label, args);
    append(std::move(instr));
}

/// @brief Emit a conditional branch with separate successor arguments.
/// @param cond SSA value determining which successor executes.
/// @param t Target block for the true edge.
/// @param targs Arguments supplied when branching to @p t.
/// @param f Target block for the false edge.
/// @param fargs Arguments supplied when branching to @p f.
/// @pre Argument counts must match the parameter lists of both targets.
/// @post Current block is marked terminated.
void IRBuilder::cbr(const il::core::Value &cond,
                    il::core::BasicBlock &t,
                    const std::vector<il::core::Value> &targs,
                    il::core::BasicBlock &f,
                    const std::vector<il::core::Value> &fargs) {
    assert(targs.size() == t.params.size() &&
           "true branch argument count must match target block parameters");
    assert(fargs.size() == f.params.size() &&
           "false branch argument count must match target block parameters");
    validateTempValue(cond, nextTemp, "condition");
    validateBranchArgs(t, targs, nextTemp, "true branch");
    validateBranchArgs(f, fargs, nextTemp, "false branch");

#ifndef NDEBUG
    // Verify condition temp ID is within bounds
    if (cond.kind == Value::Kind::Temp) {
        assert(cond.id < nextTemp && "condition temp ID exceeds allocated temporaries");
    }

    // Validate all branch argument temp IDs are within bounds
    for (const auto &arg : targs) {
        if (arg.kind == Value::Kind::Temp) {
            assert(arg.id < nextTemp &&
                   "true branch argument temp ID exceeds allocated temporaries");
        }
    }
    for (const auto &arg : fargs) {
        if (arg.kind == Value::Kind::Temp) {
            assert(arg.id < nextTemp &&
                   "false branch argument temp ID exceeds allocated temporaries");
        }
    }
#endif

    il::core::Instr instr;
    instr.op = il::core::Opcode::CBr;
    instr.type = il::core::Type(il::core::Type::Kind::Void);
    instr.operands.push_back(cond);
    instr.addBranchTarget(mod, t.label, targs);
    instr.addBranchTarget(mod, f.label, fargs);
    append(std::move(instr));
}

/// @brief Select the basic block that will receive subsequently appended instructions.
///
/// @details The builder does not clear the block's terminated flag; callers may
///          therefore inspect @ref BasicBlock::terminated to decide whether more
///          instructions may be emitted.
///
/// @param bb Block that becomes the current insertion point.
/// @post curFunc/curBlock are updated to @p bb; termination state is preserved.
void IRBuilder::setInsertPoint(il::core::BasicBlock &bb) {
    // Fast path: insertion point belongs to the current function.
    if (curFunc) {
        for (size_t i = 0; i < curFunc->blocks.size(); ++i) {
            if (&curFunc->blocks[i] == &bb) {
                curBlockIdx = i;
                return;
            }
        }
    }

    // Slow path: locate the owning function (allows switching between functions).
    for (auto &fn : mod.functions) {
        for (size_t i = 0; i < fn.blocks.size(); ++i) {
            if (&fn.blocks[i] == &bb) {
                curFunc = &fn;
                curBlockIdx = i;
                nextTemp = static_cast<unsigned>(curFunc->valueNames.size());
                return;
            }
        }
    }

    assert(false && "insert point block does not belong to any function");
    throw std::logic_error("IRBuilder: insert point block does not belong to any function");
}

/// @brief Append an instruction to the current block and update termination state.
/// @param instr Instruction being moved into the block.
/// @return Reference to the stored instruction inside the block.
/// @pre An insertion point must be established with setInsertPoint().
/// @post Terminator opcodes mark the block as finished to prevent further insertions.
il::core::Instr &IRBuilder::append(il::core::Instr instr) {
    assert(curBlockIdx.has_value() && "insert point not set");
    assert(curFunc && "no active function");
    if (!curFunc)
        throw std::logic_error("IRBuilder: no active function");
    if (!curBlockIdx)
        throw std::logic_error("IRBuilder: insert point not set");
    if (*curBlockIdx >= curFunc->blocks.size())
        throw std::logic_error("IRBuilder: insert point index is out of range");
    if (instr.op == Opcode::Count)
        throw std::logic_error("IRBuilder: instruction opcode was not initialized");

    il::core::BasicBlock &curBlock = curFunc->blocks[*curBlockIdx];
    const bool alreadyTerminated =
        !curBlock.instructions.empty() && isTerminator(curBlock.instructions.back().op);
    curBlock.terminated = alreadyTerminated;
    if (alreadyTerminated)
        throw std::logic_error("IRBuilder: cannot append instruction to terminated block");
    validateTempValues(instr.operands, nextTemp, "instruction operand");
    for (const auto &argList : instr.brArgs)
        validateTempValues(argList, nextTemp, "branch argument");
    if (instr.result) {
        constexpr unsigned kResultIdSlack = 1000;
        const unsigned maxAllowed =
            nextTemp > std::numeric_limits<unsigned>::max() - kResultIdSlack
                ? std::numeric_limits<unsigned>::max()
                : nextTemp + kResultIdSlack;
        if (*instr.result > maxAllowed) {
            throw std::logic_error("IRBuilder: result temp ID is unreasonably large");
        }
    }

#ifndef NDEBUG
    // Cannot append normal instructions after a terminator
    if (!isTerminator(instr.op)) {
        assert(!curBlock.terminated &&
               "cannot append non-terminator instruction to terminated block");
    }

    // Validate that operand temp IDs are within bounds
    for (const auto &operand : instr.operands) {
        if (operand.kind == Value::Kind::Temp) {
            assert(operand.id < nextTemp &&
                   "operand temp ID exceeds allocated temporaries (dangling reference)");
        }
    }

    // Validate result temp ID if present
    if (instr.result) {
        // Result ID should be within the next available range (allow some slack for
        // pre-reserved IDs, but catch obviously invalid values)
        assert(*instr.result <= nextTemp + 1000 &&
               "result temp ID is unreasonably large (possible corruption)");
    }
#endif

    const bool appendedTerminator = isTerminator(instr.op);
    if (appendedTerminator)
        assert(!curBlock.terminated && "block already terminated");
    instr.calleeSymbol =
        instr.callee.empty() ? il::support::Symbol{} : mod.internIdentifier(instr.callee);
    instr.labelSymbols.clear();
    instr.labelSymbols.reserve(instr.labels.size());
    for (const auto &label : instr.labels) {
        instr.labelSymbols.push_back(label.empty() ? il::support::Symbol{}
                                                   : mod.internIdentifier(label));
    }
    curBlock.instructions.push_back(std::move(instr));
    curBlock.terminated = appendedTerminator;
    return curBlock.instructions.back();
}

/// @brief Identify whether an opcode terminates a block's control flow.
///
/// @details Terminators encompass both explicit branches and exception-resume
///          operations.  Recognising them lets the builder mark blocks as closed
///          and reject additional non-phi instructions.
///
/// @param op Opcode to categorize.
/// @return True when @p op ends the block (branch, conditional branch, return, trap).
bool IRBuilder::isTerminator(il::core::Opcode op) const {
    return il::core::getOpcodeInfo(op).isTerminator;
}

/// @brief Materialize a string constant by referencing an existing global.
/// @param globalName Name of the global string to load.
/// @param loc Source location associated with the instruction.
/// @return SSA temporary containing the string value.
/// @post nextTemp advances to include the new temporary identifier.
il::core::Value IRBuilder::emitConstStr(const std::string &globalName, il::support::SourceLoc loc) {
    unsigned id = nextTemp;
    il::core::Instr instr;
    instr.result = id;
    instr.op = il::core::Opcode::ConstStr;
    instr.type = il::core::Type(il::core::Type::Kind::Str);
    instr.operands.push_back(il::core::Value::global(globalName));
    instr.loc = loc;
    append(std::move(instr));
    ++nextTemp;
    if (curFunc && curFunc->valueNames.size() <= id)
        curFunc->valueNames.resize(id + 1);
    return il::core::Value::temp(id);
}

/// @brief Emit a function call and optionally capture its result.
/// @param callee Symbol name of the function being invoked.
/// @param args Operands passed to the callee in order.
/// @param dst Optional destination SSA value pre-allocated by the caller.
/// @param loc Source location for diagnostics and debugging.
/// @throws std::logic_error If @p callee is not known to the module.
/// @post nextTemp expands when @p dst refers to a previously unseen ID.
void IRBuilder::emitCall(const std::string &callee,
                         const std::vector<il::core::Value> &args,
                         const std::optional<il::core::Value> &dst,
                         il::support::SourceLoc loc) {
    validateTempValues(args, nextTemp, "call argument");
    if (dst && dst->kind != Value::Kind::Temp)
        throw std::invalid_argument("IRBuilder: call destination must be a temporary");
#ifndef NDEBUG
    // Validate that all argument temp IDs are within bounds
    for (const auto &arg : args) {
        if (arg.kind == Value::Kind::Temp) {
            assert(arg.id < nextTemp &&
                   "call argument temp ID exceeds allocated temporaries (dangling reference)");
        }
    }
#endif

    il::core::Instr instr;
    instr.op = il::core::Opcode::Call;
    const auto it = calleeReturnTypes.find(callee);
    if (it == calleeReturnTypes.end())
        throw std::logic_error("emitCall: unknown callee '" + callee + "'");
    instr.type = it->second;
    instr.setDirectCallee(mod, callee);
    instr.operands = args;
    std::optional<unsigned> committedDst;
    if (dst) {
        instr.result = dst->id;
        if (dst->id >= nextTemp) {
            if (!curFunc)
                throw std::logic_error("IRBuilder: call destination requires an active function");
            if (dst->id == std::numeric_limits<unsigned>::max())
                throw std::overflow_error("IRBuilder: call destination temp ID overflows");
            committedDst = dst->id;
        }
    }
    instr.loc = loc;
    append(std::move(instr));
    if (committedDst) {
        nextTemp = *committedDst + 1;
        if (curFunc->valueNames.size() <= *committedDst)
            curFunc->valueNames.resize(*committedDst + 1);
    }
}

/// @brief Emit a return from the current function.
/// @param v Optional SSA value to return.
/// @param loc Source location carried by the instruction.
/// @post Marks the block as terminated and enforces the void return opcode when absent.
void IRBuilder::emitRet(const std::optional<il::core::Value> &v, il::support::SourceLoc loc) {
    if (v)
        validateTempValue(*v, nextTemp, "return value");
#ifndef NDEBUG
    // Validate return value temp ID if present
    if (v && v->kind == Value::Kind::Temp) {
        assert(v->id < nextTemp &&
               "return value temp ID exceeds allocated temporaries (dangling reference)");
    }
#endif

    il::core::Instr instr;
    instr.op = il::core::Opcode::Ret;
    instr.type = il::core::Type(il::core::Type::Kind::Void);
    if (v)
        instr.operands.push_back(*v);
    instr.loc = loc;
    append(std::move(instr));
}

/// @brief Emit a resume-same instruction for structured exception handlers.
/// @details Creates an @ref Opcode::ResumeSame terminator that rethrows the
/// current exception token to the innermost handler. The builder appends the
/// instruction to the current block, marks it terminated, and records the
/// source location for diagnostics.
/// @param token SSA value representing the exception token to resume.
/// @param loc Source location used when formatting verifier diagnostics.
void IRBuilder::emitResumeSame(const il::core::Value &token, il::support::SourceLoc loc) {
    validateTempValue(token, nextTemp, "resume token");
#ifndef NDEBUG
    // Validate token temp ID
    if (token.kind == Value::Kind::Temp) {
        assert(token.id < nextTemp &&
               "resume token temp ID exceeds allocated temporaries (dangling reference)");
    }
#endif

    il::core::Instr instr;
    instr.op = il::core::Opcode::ResumeSame;
    instr.type = il::core::Type(il::core::Type::Kind::Void);
    instr.operands.push_back(token);
    instr.loc = loc;
    append(std::move(instr));
}

/// @brief Emit a resume-next instruction for structured exception handlers.
/// @details Generates an @ref Opcode::ResumeNext terminator that forwards the
/// active exception token to the next handler in the stack. Like
/// @ref emitResumeSame, the helper appends the instruction, marks termination,
/// and records the provided source location.
/// @param token SSA value representing the exception token to resume.
/// @param loc Source location used when formatting verifier diagnostics.
void IRBuilder::emitResumeNext(const il::core::Value &token, il::support::SourceLoc loc) {
    validateTempValue(token, nextTemp, "resume token");
#ifndef NDEBUG
    // Validate token temp ID
    if (token.kind == Value::Kind::Temp) {
        assert(token.id < nextTemp &&
               "resume token temp ID exceeds allocated temporaries (dangling reference)");
    }
#endif

    il::core::Instr instr;
    instr.op = il::core::Opcode::ResumeNext;
    instr.type = il::core::Type(il::core::Type::Kind::Void);
    instr.operands.push_back(token);
    instr.loc = loc;
    append(std::move(instr));
}

/// @brief Emit a resume-label instruction transferring control to @p target.
/// @details Appends an @ref Opcode::ResumeLabel terminator that jumps to a
/// specific handler block. The token operand is preserved and the destination
/// label is recorded to maintain block parameter arity.
/// @param token SSA value representing the exception token to resume with.
/// @param target Handler block that receives control once the token is
/// resumed.
/// @param loc Source location used when formatting verifier diagnostics.
void IRBuilder::emitResumeLabel(const il::core::Value &token,
                                il::core::BasicBlock &target,
                                il::support::SourceLoc loc) {
    validateTempValue(token, nextTemp, "resume token");
#ifndef NDEBUG
    // Validate token temp ID
    if (token.kind == Value::Kind::Temp) {
        assert(token.id < nextTemp &&
               "resume token temp ID exceeds allocated temporaries (dangling reference)");
    }
#endif

    il::core::Instr instr;
    instr.op = il::core::Opcode::ResumeLabel;
    instr.type = il::core::Type(il::core::Type::Kind::Void);
    instr.operands.push_back(token);
    instr.addBranchTarget(mod, target.label);
    instr.loc = loc;
    append(std::move(instr));
}

/// @brief Reserve the next SSA temporary identifier for the currently active function.
///
/// @details Extends the value name table to ensure future debug lookups remain
///          in bounds.  The caller typically uses the returned identifier to
///          populate instructions that will be appended immediately afterwards.
///
/// @return Identifier assigned to the new temporary.
unsigned IRBuilder::reserveTempId() {
    assert(curFunc && "reserveTempId requires an active function");
    if (!curFunc)
        throw std::logic_error("IRBuilder: reserveTempId requires an active function");
    unsigned id = nextTemp++;
    if (curFunc->valueNames.size() <= id)
        curFunc->valueNames.resize(id + 1);
    return id;
}

/// @brief Associate an SSA ID with a source-facing debugger name.
/// @param id Value identifier whose name-table slot is written.
/// @param name Non-empty name stored verbatim.
/// @details Does nothing without an active function or when @p name is empty;
///          otherwise grows the active function's value-name table as needed.
void IRBuilder::setValueName(unsigned id, const std::string &name) {
    if (!curFunc || name.empty())
        return;
    if (curFunc->valueNames.size() <= id)
        curFunc->valueNames.resize(id + 1);
    curFunc->valueNames[id] = name;
}

} // namespace il::build
