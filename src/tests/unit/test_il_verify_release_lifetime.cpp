//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/test_il_verify_release_lifetime.cpp
// Purpose: Validate verifier lifetime tracking for released runtime handles.
// Key invariants: Use-after-release and double-release fail, while loop-local
//                 fresh result definitions supersede prior-iteration releases.
// Ownership/Lifetime: Constructs modules locally for verification.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

#include "il/core/BasicBlock.hpp"
#include "il/core/Extern.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Module.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/Type.hpp"
#include "il/core/Value.hpp"
#include "il/verify/Verifier.hpp"
#include "support/source_location.hpp"

#include <cassert>
#include <iostream>
#include <string>

namespace {

void appendRuntimeArrayExterns(il::core::Module &module) {
    using il::core::Extern;
    using il::core::Type;

    Extern release;
    release.name = "rt_arr_i32_release";
    release.retType = Type(Type::Kind::Void);
    release.params.push_back(Type(Type::Kind::Ptr));
    module.externs.push_back(release);

    Extern strRelease;
    strRelease.name = "rt_arr_str_release";
    strRelease.retType = Type(Type::Kind::Void);
    strRelease.params.push_back(Type(Type::Kind::Ptr));
    strRelease.params.push_back(Type(Type::Kind::I64));
    module.externs.push_back(strRelease);

    Extern len;
    len.name = "rt_arr_i32_len";
    len.retType = Type(Type::Kind::I64);
    len.params.push_back(Type(Type::Kind::Ptr));
    module.externs.push_back(len);
}

void appendManagedObjectExterns(il::core::Module &module) {
    using il::core::Extern;
    using il::core::Type;

    Extern emptyString;
    emptyString.name = "rt_str_empty";
    emptyString.retType = Type(Type::Kind::Str);
    module.externs.push_back(emptyString);

    Extern boxString;
    boxString.name = "Zanna.Core.Box.Str";
    boxString.retType = Type(Type::Kind::Ptr);
    boxString.params.push_back(Type(Type::Kind::Str));
    module.externs.push_back(boxString);

    Extern objectToString;
    objectToString.name = "Zanna.Core.Object.ToString";
    objectToString.retType = Type(Type::Kind::Str);
    objectToString.params.push_back(Type(Type::Kind::Ptr));
    module.externs.push_back(objectToString);

    Extern release;
    release.name = "rt_obj_release_check0";
    release.retType = Type(Type::Kind::I1);
    release.params.push_back(Type(Type::Kind::Ptr));
    module.externs.push_back(release);

    Extern destructorDispatch;
    destructorDispatch.name = "__zia_dtor_dispatch";
    destructorDispatch.retType = Type(Type::Kind::Void);
    destructorDispatch.params.push_back(Type(Type::Kind::Ptr));
    module.externs.push_back(destructorDispatch);

    Extern freeObject;
    freeObject.name = "rt_obj_free";
    freeObject.retType = Type(Type::Kind::Void);
    freeObject.params.push_back(Type(Type::Kind::Ptr));
    module.externs.push_back(freeObject);
}

} // namespace

int main() {
    using namespace il::core;

    {
        Module module;
        appendRuntimeArrayExterns(module);

        Function fn;
        fn.name = "use_after";
        fn.retType = Type(Type::Kind::Void);

        BasicBlock entry;
        entry.label = "entry";

        Instr makeNull;
        makeNull.result = 0;
        makeNull.op = Opcode::ConstNull;
        makeNull.type = Type(Type::Kind::Ptr);
        makeNull.loc = {1, 1};

        Instr releaseHandle;
        releaseHandle.op = Opcode::Call;
        releaseHandle.type = Type(Type::Kind::Void);
        releaseHandle.callee = "rt_arr_i32_release";
        releaseHandle.operands.push_back(Value::temp(0));
        releaseHandle.loc = {2, 1};

        Instr lenCall;
        lenCall.result = 1;
        lenCall.op = Opcode::Call;
        lenCall.type = Type(Type::Kind::I64);
        lenCall.callee = "rt_arr_i32_len";
        lenCall.operands.push_back(Value::temp(0));
        lenCall.loc = {3, 1};

        Instr ret;
        ret.op = Opcode::Ret;
        ret.type = Type(Type::Kind::Void);
        ret.loc = {4, 1};

        entry.instructions.push_back(makeNull);
        entry.instructions.push_back(releaseHandle);
        entry.instructions.push_back(lenCall);
        entry.instructions.push_back(ret);
        entry.terminated = true;

        fn.blocks.push_back(entry);
        module.functions.push_back(fn);

        auto result = il::verify::Verifier::verify(module);
        assert(!result && "use after release must fail verification");
        if (result.error().message.find("use after release") == std::string::npos)
            std::cerr << "unexpected verifier message: " << result.error().message << '\n';
        assert(result.error().message.find("use after release") != std::string::npos);
    }

    {
        Module module;
        appendRuntimeArrayExterns(module);

        Function fn;
        fn.name = "double_release";
        fn.retType = Type(Type::Kind::Void);

        BasicBlock entry;
        entry.label = "entry";

        Instr makeNull;
        makeNull.result = 0;
        makeNull.op = Opcode::ConstNull;
        makeNull.type = Type(Type::Kind::Ptr);
        makeNull.loc = {1, 1};

        Instr firstRelease;
        firstRelease.op = Opcode::Call;
        firstRelease.type = Type(Type::Kind::Void);
        firstRelease.callee = "rt_arr_i32_release";
        firstRelease.operands.push_back(Value::temp(0));
        firstRelease.loc = {2, 1};

        Instr secondRelease;
        secondRelease.op = Opcode::Call;
        secondRelease.type = Type(Type::Kind::Void);
        secondRelease.callee = "rt_arr_i32_release";
        secondRelease.operands.push_back(Value::temp(0));
        secondRelease.loc = {3, 1};

        Instr ret;
        ret.op = Opcode::Ret;
        ret.type = Type(Type::Kind::Void);
        ret.loc = {4, 1};

        entry.instructions.push_back(makeNull);
        entry.instructions.push_back(firstRelease);
        entry.instructions.push_back(secondRelease);
        entry.instructions.push_back(ret);
        entry.terminated = true;

        fn.blocks.push_back(entry);
        module.functions.push_back(fn);

        auto result = il::verify::Verifier::verify(module);
        assert(!result && "double release must fail verification");
        assert(result.error().message.find("double release") != std::string::npos);
    }

    {
        Module module;
        appendRuntimeArrayExterns(module);

        Function fn;
        fn.name = "cross_block_use_after";
        fn.retType = Type(Type::Kind::Void);

        BasicBlock entry;
        entry.label = "entry";

        Instr makeNull;
        makeNull.result = 0;
        makeNull.op = Opcode::ConstNull;
        makeNull.type = Type(Type::Kind::Ptr);

        Instr releaseHandle;
        releaseHandle.op = Opcode::Call;
        releaseHandle.type = Type(Type::Kind::Void);
        releaseHandle.callee = "rt_arr_i32_release";
        releaseHandle.operands.push_back(Value::temp(0));

        Instr br;
        br.op = Opcode::Br;
        br.type = Type(Type::Kind::Void);
        br.labels.push_back("use");
        br.brArgs.emplace_back();

        entry.instructions = {makeNull, releaseHandle, br};
        entry.terminated = true;

        BasicBlock use;
        use.label = "use";
        Instr lenCall;
        lenCall.result = 1;
        lenCall.op = Opcode::Call;
        lenCall.type = Type(Type::Kind::I64);
        lenCall.callee = "rt_arr_i32_len";
        lenCall.operands.push_back(Value::temp(0));

        Instr ret;
        ret.op = Opcode::Ret;
        ret.type = Type(Type::Kind::Void);
        use.instructions = {lenCall, ret};
        use.terminated = true;

        fn.blocks.push_back(entry);
        fn.blocks.push_back(use);
        module.functions.push_back(fn);

        auto result = il::verify::Verifier::verify(module);
        assert(!result && "cross-block use after release must fail verification");
        assert(result.error().message.find("use after release") != std::string::npos);
    }

    {
        Module module;
        appendRuntimeArrayExterns(module);

        Function fn;
        fn.name = "string_double_release";
        fn.retType = Type(Type::Kind::Void);

        BasicBlock entry;
        entry.label = "entry";

        Instr makeNull;
        makeNull.result = 0;
        makeNull.op = Opcode::ConstNull;
        makeNull.type = Type(Type::Kind::Ptr);

        Instr firstRelease;
        firstRelease.op = Opcode::Call;
        firstRelease.type = Type(Type::Kind::Void);
        firstRelease.callee = "rt_arr_str_release";
        firstRelease.operands.push_back(Value::temp(0));
        firstRelease.operands.push_back(Value::constInt(0));

        Instr secondRelease = firstRelease;

        Instr ret;
        ret.op = Opcode::Ret;
        ret.type = Type(Type::Kind::Void);

        entry.instructions = {makeNull, firstRelease, secondRelease, ret};
        entry.terminated = true;

        fn.blocks.push_back(entry);
        module.functions.push_back(fn);

        auto result = il::verify::Verifier::verify(module);
        assert(!result && "string array double release must fail verification");
        assert(result.error().message.find("double release") != std::string::npos);
    }

    {
        Module module;
        appendManagedObjectExterns(module);

        Function fn;
        fn.name = "loop_fresh_result_after_release";
        fn.retType = Type(Type::Kind::Void);

        BasicBlock entry;
        entry.label = "entry";
        Instr enterLoop;
        enterLoop.op = Opcode::Br;
        enterLoop.type = Type(Type::Kind::Void);
        enterLoop.labels = {"body"};
        enterLoop.brArgs = {{}};
        entry.instructions = {enterLoop};
        entry.terminated = true;

        BasicBlock body;
        body.label = "body";

        Instr makeString;
        makeString.result = 0;
        makeString.op = Opcode::Call;
        makeString.type = Type(Type::Kind::Str);
        makeString.callee = "rt_str_empty";

        // Box.Str both retains its input and returns a fresh object. The retain
        // classification must not prevent %1 from killing the release state
        // carried around the loop backedge for its prior dynamic instance.
        Instr boxString;
        boxString.result = 1;
        boxString.op = Opcode::Call;
        boxString.type = Type(Type::Kind::Ptr);
        boxString.callee = "Zanna.Core.Box.Str";
        boxString.operands = {Value::temp(0)};

        Instr useObject;
        useObject.result = 2;
        useObject.op = Opcode::Call;
        useObject.type = Type(Type::Kind::Str);
        useObject.callee = "Zanna.Core.Object.ToString";
        useObject.operands = {Value::temp(1)};

        Instr releaseObject;
        releaseObject.result = 3;
        releaseObject.op = Opcode::Call;
        releaseObject.type = Type(Type::Kind::I1);
        releaseObject.callee = "rt_obj_release_check0";
        releaseObject.operands = {Value::temp(1)};

        Instr chooseFinalizer;
        chooseFinalizer.op = Opcode::CBr;
        chooseFinalizer.type = Type(Type::Kind::Void);
        chooseFinalizer.operands = {Value::temp(3)};
        chooseFinalizer.labels = {"destroy", "continue"};
        chooseFinalizer.brArgs = {{}, {}};
        body.instructions = {makeString, boxString, useObject, releaseObject, chooseFinalizer};
        body.terminated = true;

        BasicBlock destroy;
        destroy.label = "destroy";
        Instr dispatchDestructor;
        dispatchDestructor.op = Opcode::Call;
        dispatchDestructor.type = Type(Type::Kind::Void);
        dispatchDestructor.callee = "__zia_dtor_dispatch";
        dispatchDestructor.operands = {Value::temp(1)};
        Instr freeObject;
        freeObject.op = Opcode::Call;
        freeObject.type = Type(Type::Kind::Void);
        freeObject.callee = "rt_obj_free";
        freeObject.operands = {Value::temp(1)};
        Instr finishDestroy;
        finishDestroy.op = Opcode::Br;
        finishDestroy.type = Type(Type::Kind::Void);
        finishDestroy.labels = {"continue"};
        finishDestroy.brArgs = {{}};
        destroy.instructions = {dispatchDestructor, freeObject, finishDestroy};
        destroy.terminated = true;

        BasicBlock continueBlock;
        continueBlock.label = "continue";
        Instr loopOrExit;
        loopOrExit.op = Opcode::CBr;
        loopOrExit.type = Type(Type::Kind::Void);
        loopOrExit.operands = {Value::constBool(true)};
        loopOrExit.labels = {"body", "exit"};
        loopOrExit.brArgs = {{}, {}};
        continueBlock.instructions = {loopOrExit};
        continueBlock.terminated = true;

        BasicBlock exit;
        exit.label = "exit";
        Instr ret;
        ret.op = Opcode::Ret;
        ret.type = Type(Type::Kind::Void);
        exit.instructions = {ret};
        exit.terminated = true;

        fn.blocks.push_back(entry);
        fn.blocks.push_back(body);
        fn.blocks.push_back(destroy);
        fn.blocks.push_back(continueBlock);
        fn.blocks.push_back(exit);
        module.functions.push_back(fn);

        auto result = il::verify::Verifier::verify(module);
        if (!result)
            std::cerr << "unexpected verifier message: " << result.error().message << '\n';
        assert(result && "fresh loop result must supersede its prior-iteration release");
    }

    // A block parameter rebinds a fresh dynamic value on every entry, exactly
    // like a result temp. Releasing one inside a loop (the inliner's
    // continuation blocks routinely carry a caller temp as a param) must not
    // report the prior iteration's release as a double release.
    {
        Module module;
        appendManagedObjectExterns(module);

        Extern strReleaseMaybe;
        strReleaseMaybe.name = "rt_str_release_maybe";
        strReleaseMaybe.retType = Type(Type::Kind::Void);
        strReleaseMaybe.params.push_back(Type(Type::Kind::Str));
        module.externs.push_back(strReleaseMaybe);

        Function fn;
        fn.name = "loop_fresh_param_after_release";
        fn.retType = Type(Type::Kind::Void);

        BasicBlock entry;
        entry.label = "entry";
        Instr enterLoop;
        enterLoop.op = Opcode::Br;
        enterLoop.type = Type(Type::Kind::Void);
        enterLoop.labels = {"head"};
        enterLoop.brArgs = {{}};
        entry.instructions = {enterLoop};
        entry.terminated = true;

        BasicBlock head;
        head.label = "head";
        Instr makeString;
        makeString.result = 0;
        makeString.op = Opcode::Call;
        makeString.type = Type(Type::Kind::Str);
        makeString.callee = "rt_str_empty";
        Instr enterHand;
        enterHand.op = Opcode::Br;
        enterHand.type = Type(Type::Kind::Void);
        enterHand.labels = {"hand"};
        enterHand.brArgs = {{Value::temp(0)}};
        head.instructions = {makeString, enterHand};
        head.terminated = true;

        BasicBlock hand;
        hand.label = "hand";
        Param handParam;
        handParam.name = "carried";
        handParam.type = Type(Type::Kind::Str);
        handParam.id = 1;
        hand.params.push_back(handParam);
        Instr releaseCarried;
        releaseCarried.op = Opcode::Call;
        releaseCarried.type = Type(Type::Kind::Void);
        releaseCarried.callee = "rt_str_release_maybe";
        releaseCarried.operands = {Value::temp(1)};
        Instr loopOrExit;
        loopOrExit.op = Opcode::CBr;
        loopOrExit.type = Type(Type::Kind::Void);
        loopOrExit.operands = {Value::constBool(true)};
        loopOrExit.labels = {"head", "exit"};
        loopOrExit.brArgs = {{}, {}};
        hand.instructions = {releaseCarried, loopOrExit};
        hand.terminated = true;

        BasicBlock exit;
        exit.label = "exit";
        Instr ret;
        ret.op = Opcode::Ret;
        ret.type = Type(Type::Kind::Void);
        exit.instructions = {ret};
        exit.terminated = true;

        fn.blocks.push_back(entry);
        fn.blocks.push_back(head);
        fn.blocks.push_back(hand);
        fn.blocks.push_back(exit);
        module.functions.push_back(fn);

        auto result = il::verify::Verifier::verify(module);
        if (!result)
            std::cerr << "unexpected verifier message: " << result.error().message << '\n';
        assert(result && "fresh block param must supersede its prior-iteration release");
    }

    return 0;
}
