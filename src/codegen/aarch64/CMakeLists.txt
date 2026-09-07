add_library(il_codegen_aarch64 STATIC
        TargetAArch64.cpp
        AsmEmitter.cpp
        RodataPool.cpp
        FrameBuilder.cpp
        MachineIR.cpp
        LowerILToMIR.cpp
        LivenessAnalysis.cpp
        InstrEffects.cpp
        MirVerify.cpp
        TerminatorLowering.cpp
        InstrLowering.cpp
        OpcodeDispatch.cpp
        FastPaths.cpp
        PreRegAllocOpt.cpp
        fastpaths/FastPaths_Arithmetic.cpp
        fastpaths/FastPaths_Call.cpp
        fastpaths/FastPaths_Cast.cpp
        fastpaths/FastPaths_Memory.cpp
        fastpaths/FastPaths_Return.cpp
        LowerOvf.cpp
        RegAllocLinear.cpp
        # Modular register allocator sub-components (Phase 4C)
        ra/RegPools.cpp
        ra/OperandRoles.cpp
        ra/Allocator.cpp
        ra/Liveness.cpp
        Coalescer.cpp
        Peephole.cpp
        # Modular peephole sub-passes (Phase 4B)
        peephole/PeepholeCommon.cpp
        peephole/Dominators.cpp
        peephole/IdentityElim.cpp
        peephole/StrengthReduce.cpp
        peephole/CopyPropDCE.cpp
        peephole/BranchOpt.cpp
        peephole/MemoryOpt.cpp
        peephole/LoopOpt.cpp
        # Modular PassManager passes (Priority 2F)
        passes/LoweringPass.cpp
        passes/LegalizePass.cpp
        passes/PreRegAllocOptPass.cpp
        passes/RegAllocPass.cpp
        passes/PeepholePass.cpp
        passes/EmitPass.cpp
        passes/BinaryEmitPass.cpp
        passes/SchedulerPass.cpp
        passes/BlockLayoutPass.cpp
        CodegenPipeline.cpp
        binenc/A64BinaryEncoder.cpp)
target_include_directories(il_codegen_aarch64 PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../..>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/zanna>)
target_link_libraries(il_codegen_aarch64
        PUBLIC
        zanna_codegen_common
        il_transform
        il_tools_common
        PRIVATE
        zanna_common_opts)
set_target_properties(il_codegen_aarch64 PROPERTIES LINKER_LANGUAGE CXX)

# --- Generate OpcodeDispatch from JSON (no Python dependency) ---
set(ZANNA_A64_SPEC "${PROJECT_SOURCE_DIR}/docs/specs/aarch64_encodings.json")
set(ZANNA_A64_OUTDIR "${PROJECT_SOURCE_DIR}/src/codegen/aarch64/generated")
set(ZANNA_A64_DISP "${ZANNA_A64_OUTDIR}/OpcodeDispatch.inc")

add_custom_command(
        OUTPUT ${ZANNA_A64_DISP}
        COMMAND ${CMAKE_COMMAND}
        -DSPEC_FILE=${ZANNA_A64_SPEC}
        -DOUTPUT_FILE=${ZANNA_A64_DISP}
        -P ${PROJECT_SOURCE_DIR}/cmake/GenAArch64Dispatch.cmake
        DEPENDS ${ZANNA_A64_SPEC} ${PROJECT_SOURCE_DIR}/cmake/GenAArch64Dispatch.cmake
        COMMENT "Generating AArch64 OpcodeDispatch.inc from JSON"
        VERBATIM)

add_custom_target(gen_aarch64_dispatch DEPENDS ${ZANNA_A64_DISP})
add_dependencies(il_codegen_aarch64 gen_aarch64_dispatch generate_runtime_registry)
