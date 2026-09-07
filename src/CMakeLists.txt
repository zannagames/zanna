#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: src/CMakeLists.txt
# Purpose: Define Zanna libraries, tools, generated sources, and install rules.
#
# Key invariants: Installed targets form a complete cross-platform developer SDK.
#
# Ownership/Lifetime: CMake owns target outputs under the selected build tree.
#
# Links: CMakeLists.txt, docs/internals/codemap.md
#
#===----------------------------------------------------------------------===#

add_subdirectory(support)

#===----------------------------------------------------------------------===//
# rtgen: Runtime registry generator
#===----------------------------------------------------------------------===//
# Build the generator tool that creates .inc files from runtime.def
add_executable(rtgen tools/rtgen/rtgen.cpp)
target_compile_features(rtgen PRIVATE cxx_std_17)
target_link_libraries(rtgen PRIVATE zanna_common_opts)

# Generate runtime registry files
set(RTGEN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/generated/il/runtime")
set(RTGEN_INPUT "${CMAKE_CURRENT_SOURCE_DIR}/il/runtime/runtime.def")
file(GLOB_RECURSE RTGEN_DEF_DEPS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/il/runtime/defs/*.def")
file(GLOB_RECURSE RTGEN_HEADER_DEPS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/il/runtime/RuntimeSurfacePolicy.inc")
set(RTGEN_OUTPUTS
        "${RTGEN_OUTPUT_DIR}/RuntimeNameMap.inc"
        "${RTGEN_OUTPUT_DIR}/RuntimeClasses.inc"
        "${RTGEN_OUTPUT_DIR}/RuntimeSignatures.inc"
        "${RTGEN_OUTPUT_DIR}/RuntimeNames.hpp"
        "${RTGEN_OUTPUT_DIR}/ZiaRuntimeExterns.inc"
)

add_custom_command(
        OUTPUT ${RTGEN_OUTPUTS}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${RTGEN_OUTPUT_DIR}"
        COMMAND rtgen "${RTGEN_INPUT}" "${RTGEN_OUTPUT_DIR}"
        DEPENDS rtgen "${RTGEN_INPUT}" ${RTGEN_DEF_DEPS} ${RTGEN_HEADER_DEPS}
        COMMENT "Generating runtime registry files from runtime.def"
        VERBATIM
)

add_custom_target(generate_runtime_registry DEPENDS ${RTGEN_OUTPUTS})

add_custom_target(generate_runtime_reference
        COMMAND rtgen --docs "${RTGEN_INPUT}" "${PROJECT_SOURCE_DIR}/docs/generated/runtime"
        DEPENDS rtgen "${RTGEN_INPUT}" ${RTGEN_DEF_DEPS}
        COMMENT "Generating runtime API reference documentation"
        VERBATIM)
#===----------------------------------------------------------------------===//

add_library(zanna_common STATIC
        common/RunProcess.cpp)
target_include_directories(zanna_common PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/zanna>)
target_link_libraries(zanna_common PRIVATE zanna_common_opts)

add_library(zanna_pass STATIC
        pass/PassManager.cpp)
target_include_directories(zanna_pass PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/zanna>)
target_link_libraries(zanna_pass PRIVATE zanna_common_opts)

add_subdirectory(codegen/common)
add_subdirectory(codegen/x86_64)
add_subdirectory(codegen/aarch64)

add_library(zanna_il_core STATIC
        il/core/OpcodeInfo.cpp
        il/core/OpcodeNames.cpp
        il/core/Type.cpp
        il/core/Value.cpp
        il/core/Instr.cpp
        il/core/BasicBlock.cpp
        il/core/Function.cpp
        il/core/Global.cpp
        il/core/Extern.cpp
        il/core/Module.cpp
)
target_include_directories(zanna_il_core PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/zanna>
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
)
target_link_libraries(zanna_il_core PRIVATE zanna_support zanna_common_opts)

## Runtime signatures: generated from runtime.def via rtgen

add_library(il_runtime STATIC
        il/runtime/RuntimeSignatures.cpp
        il/runtime/RuntimeSignatures_Handlers.cpp
        il/runtime/RuntimeSignatureParser.cpp
        il/runtime/signatures/Registry.cpp
        il/runtime/signatures/Signatures_FileIO.cpp
        il/runtime/signatures/Signatures_Strings.cpp
        il/runtime/signatures/Signatures_Math.cpp
        il/runtime/signatures/Signatures_Arrays.cpp
        il/runtime/signatures/Signatures_OOP.cpp
        il/runtime/classes/RuntimeClasses.cpp)
add_dependencies(il_runtime generate_runtime_registry)
target_link_libraries(il_runtime PUBLIC zanna_il_core zanna_runtime PRIVATE zanna_common_opts)
target_compile_definitions(il_runtime PUBLIC ZANNA_RUNTIME_NS_DUAL=$<BOOL:${ZANNA_RUNTIME_NS_DUAL}>)
target_include_directories(il_runtime PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/zanna>
)
# Increase constexpr step limit for large generated runtime descriptor tables.
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
    if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set_source_files_properties(il/runtime/RuntimeSignatures.cpp
            PROPERTIES COMPILE_FLAGS "/clang:-fconstexpr-steps=100000000")
    else ()
        set_source_files_properties(il/runtime/RuntimeSignatures.cpp
            PROPERTIES COMPILE_FLAGS "-fconstexpr-steps=100000000")
    endif ()
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # 100000000 is GCC's own default and no longer covers the descriptor sort,
    # so this budget has to be raised well above it.
    set_source_files_properties(il/runtime/RuntimeSignatures.cpp
            PROPERTIES COMPILE_FLAGS "-fconstexpr-ops-limit=1000000000")
elseif (MSVC)
    set_source_files_properties(il/runtime/RuntimeSignatures.cpp
            PROPERTIES COMPILE_FLAGS "/constexpr:steps100000000 /bigobj")
endif ()

add_library(il_build STATIC il/build/IRBuilder.cpp)
target_link_libraries(il_build
        PUBLIC
        zanna_il_core
        PRIVATE
        zanna_support
        zanna_common_opts)
target_include_directories(il_build PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/zanna>
)

add_subdirectory(il/io)

add_library(zanna_il_verify STATIC
        il/verify/DiagFormat.cpp
        il/verify/DiagSink.cpp
        il/verify/EhChecks.cpp
        il/verify/EhModel.cpp
        il/verify/EhVerifier.cpp
        il/verify/Verifier.cpp
        il/verify/ExternVerifier.cpp
        il/verify/GlobalVerifier.cpp
        il/verify/FunctionVerifier.cpp
        il/verify/InstructionCheckUtils.cpp
        il/verify/InstructionChecker.cpp
        il/verify/generated/SpecTables.cpp
        il/verify/InstructionChecker_Arithmetic.cpp
        il/verify/InstructionChecker_Memory.cpp
        il/verify/InstructionChecker_Runtime.cpp
        il/verify/OperandCountChecker.cpp
        il/verify/OperandTypeChecker.cpp
        il/verify/ResultTypeChecker.cpp
        il/verify/InstructionStrategies.cpp
        il/verify/ExceptionHandlerAnalysis.cpp
        il/verify/BranchVerifier.cpp
        il/verify/ControlFlowChecker.cpp
        il/verify/VerifierTable.cpp
        il/verify/TypeInference.cpp)
target_link_libraries(zanna_il_verify
        PUBLIC
        zanna_il_core
        il_runtime
        zanna_il_io
        PRIVATE
        il_analysis
        zanna_common_opts
)
target_include_directories(zanna_il_verify PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/zanna>
)

add_library(zanna_il_full INTERFACE)
target_link_libraries(zanna_il_full INTERFACE
        zanna_il_core
        zanna_il_verify
        zanna_il_io
        zanna_support
)

add_library(zanna::support ALIAS zanna_support)
add_library(zanna::il_core ALIAS zanna_il_core)
add_library(zanna::il_verify ALIAS zanna_il_verify)
add_library(zanna::il_io ALIAS zanna_il_io)
add_library(zanna::il_full ALIAS zanna_il_full)

add_library(il_api STATIC il/api/expected_api.cpp)
target_link_libraries(il_api
        PUBLIC zanna_il_full
        PRIVATE zanna_common_opts)
target_include_directories(il_api PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/zanna>
)

add_library(il_analysis STATIC
        il/analysis/CFG.cpp
        il/analysis/CallGraph.cpp
        il/analysis/Dominators.cpp
        il/analysis/IntRangeAnalysis.cpp
        il/analysis/MemorySSA.cpp)
target_link_libraries(il_analysis PUBLIC zanna_il_core PRIVATE zanna_common_opts)
target_include_directories(il_analysis PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/zanna>
)

add_library(il_transform STATIC
        il/transform/AnalysisManager.cpp
        il/transform/PassManager.cpp
        il/transform/PassRegistry.cpp
        il/transform/PipelineExecutor.cpp
        il/transform/CheckOpt.cpp
        il/transform/ArrayFastPathOpt.cpp
        il/transform/Devirtualize.cpp
        il/transform/IfConvert.cpp
        il/transform/LateCleanup.cpp
        il/transform/Inline.cpp
        il/transform/OwnershipOpt.cpp
        il/transform/RuntimeFastPathOpt.cpp
        il/transform/Peephole.cpp
        il/transform/ConstFold.cpp
        il/transform/DCE.cpp
        il/transform/Mem2Reg.cpp
        il/transform/EarlyCSE.cpp
        il/transform/EHOpt.cpp
        il/transform/DSE.cpp
        il/transform/LoopSimplify.cpp
        il/transform/LoopRotate.cpp
        il/transform/LICM.cpp
        il/transform/IndVarSimplify.cpp
        il/transform/LoopUnroll.cpp
        il/transform/GVN.cpp
        il/transform/ValueKey.cpp
        il/transform/SCCP.cpp
        il/transform/Reassociate.cpp
        il/transform/SiblingRecursion.cpp
        il/transform/SimplifyCFG.cpp
        il/transform/SimplifyCFG/BlockMerging.cpp
        il/transform/SimplifyCFG/BranchFolding.cpp
        il/transform/SimplifyCFG/ForwardingElimination.cpp
        il/transform/SimplifyCFG/JumpThreading.cpp
        il/transform/SimplifyCFG/ParamCanonicalization.cpp
        il/transform/SimplifyCFG/PassContext.cpp
        il/transform/SimplifyCFG/ReachabilityCleanup.cpp
        il/transform/SimplifyCFG/Utils.cpp
        il/transform/analysis/Liveness.cpp
        il/transform/analysis/LoopInfo.cpp)
target_link_libraries(il_transform
        PUBLIC
        zanna_il_core
        zanna_pass
        PRIVATE
        zanna_il_verify
        il_analysis
        il_utils
        zanna_common_opts)
target_include_directories(il_transform PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/zanna>
)

add_library(il_utils STATIC
        il/utils/Utils.cpp
        il/utils/UseDefInfo.cpp)
target_link_libraries(il_utils PUBLIC zanna_il_core PRIVATE zanna_common_opts)
target_include_directories(il_utils PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/zanna>
)

add_library(il_link STATIC
        il/link/ModuleLinker.cpp
        il/link/InteropThunks.cpp)
target_link_libraries(il_link PUBLIC zanna_il_core PRIVATE zanna_common_opts)
target_include_directories(il_link PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/zanna>
)

add_library(il_tools_common STATIC
        tools/common/module_loader.cpp
        tools/common/source_loader.cpp
        tools/common/project_loader.cpp
        tools/common/vm_executor.cpp
        tools/common/asset/AssetCompiler.cpp
)
target_link_libraries(il_tools_common PUBLIC il_api zanna_bytecode PRIVATE zanna_common_opts zanna_packaging zanna_codegen_common)

# Packaging library — self-contained C++, no runtime (zanna_rt_*) dependencies.
# Only links rt_crc32.o directly (no GC dependency, pure function).
add_library(zanna_packaging STATIC
        tools/common/packaging/CpioWriter.cpp
        tools/common/packaging/PkgDeflate.cpp
        tools/common/packaging/PkgGzip.cpp
        tools/common/packaging/PkgHash.cpp
        tools/common/packaging/PkgMD5.cpp
        tools/common/packaging/PkgPNG.cpp
        tools/common/packaging/PkgZlib.cpp
        tools/common/packaging/ZipWriter.cpp
        tools/common/packaging/ArWriter.cpp
        tools/common/packaging/TarWriter.cpp
        tools/common/packaging/PlistGenerator.cpp
        tools/common/packaging/DesktopEntryGenerator.cpp
        tools/common/packaging/IconGenerator.cpp
        tools/common/packaging/MacOSPackageBuilder.cpp
        tools/common/packaging/LinuxPackageBuilder.cpp
        tools/common/packaging/LinuxRuntimeStubGen.cpp
        tools/common/packaging/ToolchainInstallManifest.cpp
        tools/common/packaging/PEBuilder.cpp
        tools/common/packaging/LnkWriter.cpp
        tools/common/packaging/WindowsPackageBuilder.cpp
        tools/common/packaging/WindowsInstallerMetadata.cpp
        tools/common/packaging/WindowsPEValidation.cpp
        tools/common/packaging/PkgVerify.cpp
        tools/common/packaging/ZipReader.cpp
        tools/common/packaging/InstallerStubGen.cpp
        tools/common/packaging/InstallerStubGenA64.cpp
        tools/common/packaging/InstallerStub.cpp
        tools/common/packaging/XarWriter.cpp
        tools/common/asset/ZpakWriter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/runtime/core/rt_crc32.c
)
target_link_libraries(zanna_packaging PRIVATE zanna_common_opts zanna_common)
target_include_directories(zanna_packaging PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tools/common/packaging>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tools/common/asset>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/runtime/core>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

if(WIN32)
    # The installer host runs before the app-local compiler runtime is deployed,
    # so it deliberately compiles the small archive/metadata subset again with
    # the static MSVC runtime instead of linking the /MD packaging library.
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _zanna_installer_processor)
    if(_zanna_installer_processor MATCHES "arm64|aarch64")
        set(ZANNA_INSTALLER_PROCESSOR_ARCH "arm64")
    else()
        set(ZANNA_INSTALLER_PROCESSOR_ARCH "amd64")
    endif()
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/windows_installer/WindowsInstallerHost.manifest.in"
        "${CMAKE_CURRENT_BINARY_DIR}/WindowsInstallerHost.manifest"
        @ONLY)
    file(TO_CMAKE_PATH
         "${CMAKE_CURRENT_BINARY_DIR}/WindowsInstallerHost.manifest"
         ZANNA_INSTALLER_MANIFEST_PATH)
    file(TO_CMAKE_PATH
         "${CMAKE_SOURCE_DIR}/misc/images/zanna-installer.ico"
         ZANNA_INSTALLER_ICON_PATH)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/windows_installer/WindowsInstallerHost.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/WindowsInstallerHost.rc"
        @ONLY)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/windows_installer/WindowsInstallerCleanup.manifest.in"
        "${CMAKE_CURRENT_BINARY_DIR}/WindowsInstallerCleanup.manifest"
        @ONLY)
    file(TO_CMAKE_PATH
         "${CMAKE_CURRENT_BINARY_DIR}/WindowsInstallerCleanup.manifest"
         ZANNA_INSTALLER_CLEANUP_MANIFEST_PATH)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/windows_installer/WindowsInstallerCleanup.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/WindowsInstallerCleanup.rc"
        @ONLY)
    add_executable(zanna_installer_host WIN32
        tools/windows_installer/main.cpp
        tools/windows_installer/WindowsInstallerHost.cpp
        tools/windows_installer/WindowsInstallerBrandDialog.cpp
        tools/windows_installer/WindowsInstallerBrandValidation.cpp
        tools/windows_installer/WindowsInstallerLifecycle.cpp
        tools/windows_installer/WindowsInstallerTheme.cpp
        tools/windows_installer/WindowsInstallerVersion.cpp
        tools/windows_installer/WindowsInstallerWizard.cpp
        tools/windows_installer/WindowsInstallerUpdate.cpp
        tools/common/packaging/WindowsInstallerMetadata.cpp
        tools/common/packaging/ZipReader.cpp
        tools/common/packaging/PkgDeflate.cpp
        tools/common/packaging/PkgHash.cpp
        tools/common/packaging/WindowsPEValidation.cpp
        runtime/core/rt_crc32.c
        "${CMAKE_CURRENT_BINARY_DIR}/WindowsInstallerHost.rc")
    set_target_properties(zanna_installer_host PROPERTIES
        OUTPUT_NAME "zanna-installer-host"
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_definitions(zanna_installer_host PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(ZANNA_INSTALLER_ENABLE_TEST_HOOKS)
        target_compile_definitions(zanna_installer_host PRIVATE
            ZANNA_INSTALLER_ENABLE_TEST_HOOKS=1)
    endif()
    target_compile_options(zanna_installer_host PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX /permissive- /utf-8 /sdl /guard:cf>)
    target_link_options(zanna_installer_host PRIVATE /guard:cf /DYNAMICBASE /NXCOMPAT /MANIFEST:NO)
    target_include_directories(zanna_installer_host PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/common/packaging"
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime/core")
    target_link_libraries(zanna_installer_host PRIVATE
        advapi32 bcrypt comctl32 dwmapi gdi32 ole32 rstrtmgr shell32 shlwapi user32 uxtheme
        version winhttp)
    # Application packaging links generated binaries against the release MSVC
    # runtime even when the compiler itself is a Debug build. Mirror the
    # installed app-local runtime beside the native support host so build-tree
    # package commands and installer smokes have the same dependency closure.
    if(MSVC AND ZANNA_INSTALL_MSVC_RUNTIME AND CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        add_custom_command(TARGET zanna_installer_host POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
                "$<TARGET_FILE_DIR:zanna_installer_host>"
            COMMENT "Staging app-local MSVC runtime for Windows application packaging"
            VERBATIM)
    endif()
    install(TARGETS zanna_installer_host RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    add_executable(zanna_installer_cleanup WIN32
        tools/windows_installer/WindowsInstallerCleanup.cpp
        tools/windows_installer/WindowsInstallerCleanupPolicy.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/WindowsInstallerCleanup.rc")
    set_target_properties(zanna_installer_cleanup PROPERTIES
        OUTPUT_NAME "zanna-installer-cleanup"
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_definitions(zanna_installer_cleanup PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(zanna_installer_cleanup PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX /permissive- /utf-8 /sdl /guard:cf>)
    target_link_options(zanna_installer_cleanup PRIVATE
        /guard:cf /DYNAMICBASE /NXCOMPAT /MANIFEST:NO)
    target_link_libraries(zanna_installer_cleanup PRIVATE shell32)
    install(TARGETS zanna_installer_cleanup RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
target_include_directories(il_tools_common PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/zanna>
)

add_library(il_vm STATIC
        vm/Runner.cpp
        vm/VM.cpp
        vm/DispatchStrategy.cpp
        vm/tco.cpp
        vm/FunctionExecCache.cpp
        vm/debug/VM_DebugUtils.cpp
        # VMInit.cpp is included directly in Runner.cpp to avoid Windows static lib linking issues
        vm/VMContext.cpp
        vm/ThreadsRuntime.cpp
        vm/NetworkRuntime.cpp
        vm/FunctionalRuntime.cpp
        vm/Game3DRuntime.cpp
        vm/debug/VMDebug.cpp
        vm/RuntimeBridge.cpp
        vm/Marshal.cpp
        vm/DiagFormat.cpp
        vm/OpHandlers.cpp
        vm/OpHandlerUtils.cpp
        vm/OpHelpers.cpp
        vm/Trap.cpp
        vm/err_bridge.cpp
        vm/debug/Debug.cpp
        vm/debug/DebugScript.cpp
        vm/debug/Trace.cpp
        vm/mem_ops.cpp
        vm/int_ops_arith.cpp
        vm/int_ops_cmp.cpp
        vm/int_ops_convert.cpp
        vm/fp_ops.cpp
        vm/control_flow.cpp
        vm/ops/common/Branching.cpp
        vm/ops/Op_BranchSwitch.cpp
        vm/ops/Op_CallRet.cpp
        vm/ops/Op_TrapEh.cpp)
target_include_directories(il_vm PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/zanna>
)
target_link_libraries(il_vm
        PUBLIC
        zanna_il_core
        il_runtime
        zanna_runtime
        PRIVATE
        zanna_support
        zanna_common_opts)

# Enable tail-call optimisation across the VM by default.
option(IL_VM_ENABLE_TAILCALL "Enable VM tail-call optimization by default" ON)
if (IL_VM_ENABLE_TAILCALL)
    target_compile_definitions(il_vm PUBLIC ZANNA_VM_TAILCALL=1)
endif ()

if (ZANNA_VM_THREADED)
    target_compile_definitions(il_vm PUBLIC ZANNA_VM_THREADED=1)
endif ()

# Bytecode VM - fast interpreter using compact bytecode format
add_library(zanna_bytecode STATIC
        bytecode/Bytecode.cpp
        bytecode/BytecodeCompiler.cpp
        bytecode/BytecodeVM.cpp
        bytecode/BytecodeVM_threaded.cpp)
target_include_directories(zanna_bytecode PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/zanna>)
target_link_libraries(zanna_bytecode
        PUBLIC zanna_il_core zanna_il_verify
        PRIVATE zanna_support il_vm zanna_common_opts)

add_library(il_codegen_x86_64 STATIC
        codegen/x86_64/AsmEmitter.cpp
        codegen/x86_64/Backend.cpp
        codegen/x86_64/CallLowering.cpp
        codegen/x86_64/FrameLowering.cpp
        codegen/x86_64/ISel.cpp
        codegen/x86_64/LowerDiv.cpp
        codegen/x86_64/LowerILToMIR.cpp
        codegen/x86_64/MachineIR.cpp
        codegen/x86_64/Peephole.cpp
        codegen/x86_64/RegAllocLinear.cpp
        codegen/x86_64/ra/Allocator.cpp
        codegen/x86_64/ra/Coalescer.cpp
        codegen/x86_64/ra/LiveIntervals.cpp
        codegen/x86_64/ra/Spiller.cpp
        codegen/x86_64/TargetX64.cpp
        common/Mangle.cpp)
set_target_properties(il_codegen_x86_64 PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(il_codegen_x86_64 PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/zanna>)
target_link_libraries(il_codegen_x86_64 PRIVATE zanna_common_opts)
add_dependencies(il_codegen_x86_64 gen_x86_64_encodings generate_runtime_registry)

add_library(zanna_cmd_arm64 STATIC
        tools/zanna/cmd_codegen_arm64.cpp)
target_link_libraries(zanna_cmd_arm64 PRIVATE zanna_common_opts zanna_common zanna_codegen_common il_tools_common il_codegen_aarch64 il_transform zanna::il_full)

add_library(zanna_native_compiler STATIC
        tools/common/native_compiler.cpp)
target_link_libraries(zanna_native_compiler
        PUBLIC il_tools_common
        PRIVATE zanna_codegen_x86_64 il_codegen_aarch64 zanna_common)
target_include_directories(zanna_native_compiler PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

# REPL library (custom line editor, session, adapters)
add_library(zanna_repl STATIC
        repl/ReplLineEditor.cpp
        repl/ReplHistoryCodec.cpp
        repl/ReplInputClassifier.cpp
        repl/ReplMetaCommands.cpp
        repl/ReplOutputCapture.cpp
        repl/ReplSession.cpp
        repl/ZiaReplAdapter.cpp
        repl/BasicReplAdapter.cpp)
target_include_directories(zanna_repl PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_link_libraries(zanna_repl
        PUBLIC
        zanna_common_opts
        tui
        zia_editor_services
        fe_basic
        zanna::il_full
        zanna_bytecode)

# Build zanna (CLI) sources list conditionally based on enabled frontends
set(ZANNA_CLI_SOURCES
        tools/zanna/main.cpp
        tools/zanna/cmd_run.cpp
        tools/zanna/DebugAdapter.cpp
        tools/zanna/cmd_run_il.cpp
        tools/zanna/cmd_front_basic.cpp
        tools/zanna/cmd_front_zia.cpp
        tools/zanna/cmd_il_opt.cpp
        tools/zanna/cmd_codegen_x64.cpp
        tools/zanna/cmd_bench.cpp
        tools/zanna/cmd_init.cpp
        tools/zanna/cmd_asset.cpp
        tools/zanna/cmd_package.cpp
        tools/zanna/cmd_install_package.cpp
        tools/zanna/cmd_repl.cpp
        tools/zanna/cmd_eval.cpp
        tools/zanna/cmd_explain.cpp
        tools/zanna/cli.cpp
        tools/zanna/break_spec.cpp)

set(ZANNA_CLI_LINK_LIBS
        zanna_common_opts
        zanna::il_full
        il_vm
        il_transform
        il_link
        il_api
        il_tools_common
        zanna_native_compiler
        zanna_codegen_x86_64
        il_codegen_aarch64
        zanna_cmd_arm64
        zanna_bytecode
        zanna_packaging
        zanna_repl
        lsp_common_lib)

add_executable(zanna ${ZANNA_CLI_SOURCES})
if (WIN32)
    target_sources(zanna PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/common/WinDialogSuppress.c)
endif ()
set_target_properties(zanna PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/src/tools/zanna)
target_link_libraries(zanna PRIVATE ${ZANNA_CLI_LINK_LIBS})
# The `zanna asset` subcommand drives the graphics runtime's model import and
# scene bake; mirror the runtime's graphics gate for its translation unit.
if (NOT ZANNA_GRAPHICS_MODE STREQUAL "OFF")
    target_compile_definitions(zanna PRIVATE ZANNA_ENABLE_GRAPHICS)
endif ()
if (MSVC)
    target_link_options(zanna PRIVATE /STACK:8388608)
endif ()
add_dependencies(zanna zanna_runtime_components)
get_target_property(ZANNA_LINK_LIBS zanna LINK_LIBRARIES)
message(STATUS "zanna LINK_LIBRARIES: ${ZANNA_LINK_LIBS}")
install(TARGETS zanna RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if (APPLE)
    add_executable(zanna-file-handler
            tools/macos/ZannaFileHandler.m)
    set_target_properties(zanna-file-handler PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/src/tools/macos)
    target_link_libraries(zanna-file-handler PRIVATE "-framework Cocoa")
    install(TARGETS zanna-file-handler
            RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/zanna)
endif ()

add_executable(zbasic
        tools/zbasic/main.cpp
        tools/zbasic/usage.cpp
        tools/zbasic/cli_compat.cpp
        tools/zanna/cmd_front_basic.cpp
        tools/zanna/cli.cpp)
if (WIN32)
    target_sources(zbasic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/common/WinDialogSuppress.c)
endif ()
set_target_properties(zbasic PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/src/tools/zbasic)
target_link_libraries(zbasic
        PRIVATE
        zanna_common_opts
        zanna::il_full
        il_vm
        il_transform
        il_api
        il_tools_common
        zanna_native_compiler
        zanna_codegen_x86_64
        il_codegen_aarch64
        zanna_cmd_arm64
        zanna_common
        zanna_bytecode)
install(TARGETS zbasic RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_executable(zia
        tools/zia/main.cpp
        tools/zia/usage.cpp
        tools/zia/cli_compat.cpp
        tools/zanna/cmd_front_zia.cpp
        tools/zanna/cli.cpp)
if (WIN32)
    target_sources(zia PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/common/WinDialogSuppress.c)
endif ()
set_target_properties(zia PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/src/tools/zia)
# On ELF/lld the --whole-archive group must come *before* the ordinary
# `fe_basic` / `zia_editor_services` link items below: a member that was
# already extracted lazily and is then force-loaded again is reported as a
# duplicate symbol. Declaring the group first makes the later lazy mentions
# no-ops (every member is already loaded). Apple and MSVC use link options,
# which are order-insensitive, and stay next to the explanation below.
if (UNIX AND NOT APPLE)
    target_link_libraries(zia PRIVATE
            -Wl,--whole-archive $<TARGET_FILE:zia_editor_services> $<TARGET_FILE:fe_basic> -Wl,--no-whole-archive
            il_build)
endif ()
target_link_libraries(zia
        PRIVATE
        zanna_common_opts
        zanna::il_full
        il_vm
        il_transform
        il_api
        il_tools_common
        zanna_native_compiler
        zanna_codegen_x86_64
        il_codegen_aarch64
        zanna_cmd_arm64
        zanna_common
        zanna_bytecode
        fe_basic)
target_link_libraries(zia PRIVATE zia_editor_services)
# Force-load the editor-service archives so their strong-symbol language-service
# bridges win over the weak fallbacks in zanna_runtime/core/*_completion_stub.c.
# Without force-loading, the static linker can satisfy every rt_* symbol from
# the stubs (which appear first) and never pull in the strong-symbol objects.
if (APPLE)
    target_link_options(zia PRIVATE LINKER:-force_load,$<TARGET_FILE:zia_editor_services>)
    target_link_options(zia PRIVATE LINKER:-force_load,$<TARGET_FILE:fe_basic>)
elseif (MSVC)
    target_link_options(zia PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:zia_editor_services>")
    target_link_options(zia PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:fe_basic>")
endif ()
# (ELF: see the --whole-archive group declared above the main link list.)
if (MSVC)
    target_link_options(zia PRIVATE /STACK:8388608)
endif ()
install(TARGETS zia RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

#===----------------------------------------------------------------------===//
# lsp_common_lib: Shared protocol infrastructure for language servers
#===----------------------------------------------------------------------===//
add_library(lsp_common_lib STATIC
        tools/lsp-common/Json.cpp
        tools/lsp-common/Transport.cpp
        tools/lsp-common/JsonRpc.cpp
        tools/lsp-common/DocumentStore.cpp
        tools/lsp-common/LspHandler.cpp
        tools/lsp-common/McpHandler.cpp
        tools/lsp-common/TextUtils.cpp
        tools/lsp-common/DiagnosticUtils.cpp
        tools/lsp-common/ICompilerBridge.cpp)
target_include_directories(lsp_common_lib PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_link_libraries(lsp_common_lib PUBLIC zanna_common_opts il_runtime)

#===----------------------------------------------------------------------===//
# zia_server_lib: Zia-specific compiler bridge
#===----------------------------------------------------------------------===//
add_library(zia_server_lib STATIC
        tools/zia-server/CompilerBridge.cpp)
target_include_directories(zia_server_lib PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_link_libraries(zia_server_lib
        PUBLIC
        lsp_common_lib
        zia_editor_services
        zanna::il_full
        il_transform)

add_executable(zia-server
        tools/zia-server/main.cpp)
set_target_properties(zia-server PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/src/tools/zia-server)
target_link_libraries(zia-server
        PRIVATE
        zia_server_lib)
install(TARGETS zia-server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

#===----------------------------------------------------------------------===//
# zbasic_server_lib: BASIC-specific compiler bridge
#===----------------------------------------------------------------------===//
add_library(zbasic_server_lib STATIC
        tools/zbasic-server/BasicCompilerBridge.cpp)
target_include_directories(zbasic_server_lib PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_link_libraries(zbasic_server_lib
        PUBLIC
        lsp_common_lib
        fe_basic
        zanna::il_full
        il_transform)

add_executable(zbasic-server
        tools/zbasic-server/main.cpp)
set_target_properties(zbasic-server PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/src/tools/zbasic-server)
target_link_libraries(zbasic-server
        PRIVATE
        zbasic_server_lib)
install(TARGETS zbasic-server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_executable(ilrun
        tools/ilrun/main.cpp
        tools/ilrun/cli_compat.cpp
        tools/zanna/cmd_run_il.cpp
        tools/zanna/cli.cpp
        tools/zanna/break_spec.cpp)
set_target_properties(ilrun PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/src/tools/ilrun)
target_link_libraries(ilrun
        PRIVATE
        zanna_common_opts
        zanna::il_full
        il_vm
        il_api
        il_tools_common
        zanna_bytecode)
install(TARGETS ilrun RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_executable(il-verify tools/il-verify/il-verify.cpp)
set_target_properties(il-verify PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/src/tools/il-verify)
target_link_libraries(il-verify
        PRIVATE
        zanna_common_opts
        zanna::il_full
        il_build
        il_vm
        il_api
        il_tools_common)
install(TARGETS il-verify RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_executable(il-dis tools/il-dis/main.cpp)
set_target_properties(il-dis PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/src/tools/il-dis)
target_link_libraries(il-dis
        PRIVATE
        zanna_common_opts
        zanna::il_full
        il_build
        il_api
        il_tools_common
        zanna_bytecode)
install(TARGETS il-dis RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_subdirectory(frontends/common)
add_subdirectory(frontends/basic)
add_subdirectory(frontends/zia)

target_link_libraries(il_tools_common PRIVATE fe_basic fe_zia)

add_executable(basic-ast-dump
        tools/basic-ast-dump/main.cpp
        tools/basic/common.cpp)
set_target_properties(basic-ast-dump PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/src/tools/basic-ast-dump)
target_link_libraries(basic-ast-dump PRIVATE fe_basic il_tools_common)
target_compile_definitions(basic-ast-dump PRIVATE ZANNA_BASIC_TOOL_USAGE="Usage: basic-ast-dump <file.bas>\\n")
add_executable(basic-lex-dump
        tools/basic-lex-dump/main.cpp
        tools/basic/common.cpp)
set_target_properties(basic-lex-dump PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/src/tools/basic-lex-dump)
target_link_libraries(basic-lex-dump PRIVATE fe_basic il_tools_common)
target_compile_definitions(basic-lex-dump PRIVATE ZANNA_BASIC_TOOL_USAGE="Usage: basic-lex-dump <file.bas>\\n")
install(TARGETS basic-ast-dump basic-lex-dump RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/support/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zanna/support
        FILES_MATCHING PATTERN "*.hpp"
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/il/core/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zanna/il/core
        FILES_MATCHING PATTERN "*.hpp" PATTERN "*.def"
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/il/build/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zanna/il/build
        FILES_MATCHING PATTERN "*.hpp"
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/il/io/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zanna/il/io
        FILES_MATCHING PATTERN "*.hpp"
)

install(FILES ${PROJECT_SOURCE_DIR}/include/zanna/il/io/FormatUtils.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zanna/il/io)

install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/zanna/pass/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zanna/pass
        FILES_MATCHING PATTERN "*.hpp")

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/il/verify/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zanna/il/verify
        FILES_MATCHING PATTERN "*.hpp"
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/il/analysis/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zanna/il/analysis
        FILES_MATCHING PATTERN "*.hpp"
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/il/transform/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zanna/il/transform
        FILES_MATCHING PATTERN "*.hpp"
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/il/runtime/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zanna/il/runtime
        FILES_MATCHING PATTERN "*.hpp"
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/il/api/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zanna/il/api
        FILES_MATCHING PATTERN "*.hpp"
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/il/utils/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zanna/il/utils
        FILES_MATCHING PATTERN "*.hpp"
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/vm/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zanna/vm
        FILES_MATCHING PATTERN "*.hpp"
        PATTERN "VM.hpp" EXCLUDE
)

install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/zanna/vm/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zanna/vm
        FILES_MATCHING PATTERN "*.hpp"
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/frontends/common/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zanna/frontends/common
        FILES_MATCHING PATTERN "*.hpp"
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/frontends/basic/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zanna/frontends/basic
        FILES_MATCHING PATTERN "*.hpp"
)
