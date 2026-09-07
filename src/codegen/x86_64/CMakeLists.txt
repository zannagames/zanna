add_library(zanna_codegen_x86_64 STATIC
        TargetX64.cpp
        MachineIR.cpp
        AsmEmitter.cpp
        ../../common/Mangle.cpp
        asmfmt/Format.cpp
        RegAllocLinear.cpp
        ra/Allocator.cpp
        ra/Coalescer.cpp
        ra/LiveIntervals.cpp
        ra/Spiller.cpp
        ra/Liveness.cpp
        FrameLowering.cpp
        CallLowering.cpp
        OperandRoles.cpp
        MirVerify.cpp
        PreRegAllocOpt.cpp
        ISel.cpp
        LowerDiv.cpp
        LowerOvf.cpp
        LowerILToMIR.cpp
        LoweringRuleTable.cpp
        Lowering.EmitCommon.cpp
        Lowering.Arith.cpp
        Lowering.Bitwise.cpp
        Lowering.Mem.cpp
        Lowering.EH.cpp
        Lowering.CF.cpp
        LoweringRules.cpp
        Scheduler.cpp
        Peephole.cpp
        peephole/ArithSimplify.cpp
        peephole/MovFolding.cpp
        peephole/MemoryOpt.cpp
        peephole/DCE.cpp
        peephole/BranchOpt.cpp
        Backend.cpp
        CodegenPipeline.cpp
        binenc/X64BinaryEncoder.cpp
        passes/LoweringPass.cpp
        passes/LegalizePass.cpp
        passes/PreRegAllocOptPass.cpp
        passes/RegAllocPass.cpp
        passes/SchedulerPass.cpp
        passes/PeepholePass.cpp
        passes/EmitPass.cpp
        passes/BinaryEmitPass.cpp
)

target_include_directories(zanna_codegen_x86_64 PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
)

target_link_libraries(zanna_codegen_x86_64 PUBLIC zanna_common zanna_codegen_common il_transform PRIVATE zanna_common_opts)

set_target_properties(zanna_codegen_x86_64 PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
)

# --- Generate encoding tables from JSON (no Python dependency) ---
set(ZANNA_X64_SPEC "${PROJECT_SOURCE_DIR}/docs/specs/x86_64_encodings.json")
set(ZANNA_X64_OUTDIR "${PROJECT_SOURCE_DIR}/src/codegen/x86_64/generated")
set(ZANNA_X64_ENC "${ZANNA_X64_OUTDIR}/EncodingTable.inc")
set(ZANNA_X64_OPFMT "${ZANNA_X64_OUTDIR}/OpFmtTable.inc")

add_custom_command(
        OUTPUT ${ZANNA_X64_ENC} ${ZANNA_X64_OPFMT}
        COMMAND ${CMAKE_COMMAND}
        -DSPEC_FILE=${ZANNA_X64_SPEC}
        -DOUTPUT_DIR=${ZANNA_X64_OUTDIR}
        -P ${PROJECT_SOURCE_DIR}/cmake/GenX86Encodings.cmake
        DEPENDS ${ZANNA_X64_SPEC} ${PROJECT_SOURCE_DIR}/cmake/GenX86Encodings.cmake
        COMMENT "Generating x86_64 encoding tables from JSON"
        VERBATIM)

add_custom_target(gen_x86_64_encodings DEPENDS ${ZANNA_X64_ENC} ${ZANNA_X64_OPFMT})
add_dependencies(zanna_codegen_x86_64 gen_x86_64_encodings generate_runtime_registry)
