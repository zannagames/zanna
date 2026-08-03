#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: src/tests/unit/CMakeLists.txt
# Purpose: Build and register standalone Zanna unit tests.
#
# Key invariants: Unit tests use repository targets without product dependencies.
#
# Ownership/Lifetime: CTest owns generated unit-test outputs in the build tree.
#
# Links: docs/internals/testing.md, src/tests/CMakeLists.txt
#
#===----------------------------------------------------------------------===#

set(ZANNA_UNIT_SUPPORT_LIBS zanna_support)
set(ZANNA_UNIT_SUPPORT_LIBS ${ZANNA_UNIT_SUPPORT_LIBS} PARENT_SCOPE)
set(ZANNA_RUNTIME_TEST_LIBS zanna_runtime)
set(ZANNA_RUNTIME_TEST_LIBS ${ZANNA_RUNTIME_TEST_LIBS} PARENT_SCOPE)

zanna_add_test(test_platform_capabilities test_platform_capabilities.cpp)
zanna_add_ctest(test_platform_capabilities test_platform_capabilities)
zanna_add_test_labels(test_platform_capabilities unit)

function(zanna_add_isolated_runtime_test target)
    if (NOT TARGET runtime_contract_tests)
        add_custom_target(runtime_contract_tests)
    endif ()

    add_executable(${target} ${ARGN})
    set_target_properties(${target} PROPERTIES
            C_STANDARD 11
            C_STANDARD_REQUIRED ON
            C_EXTENSIONS OFF)
    target_include_directories(${target} PRIVATE
            ${ZANNA_TESTS_DIR}
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_BINARY_DIR}/generated
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/src/lib/graphics/include
            ${CMAKE_SOURCE_DIR}/src/lib/gui/include
            ${CMAKE_SOURCE_DIR}/src/runtime
            ${CMAKE_SOURCE_DIR}/src/runtime/core
            ${CMAKE_SOURCE_DIR}/src/runtime/arrays
            ${CMAKE_SOURCE_DIR}/src/runtime/oop
            ${CMAKE_SOURCE_DIR}/src/runtime/collections
            ${CMAKE_SOURCE_DIR}/src/runtime/game
            ${CMAKE_SOURCE_DIR}/src/runtime/text
            ${CMAKE_SOURCE_DIR}/src/runtime/io
            ${CMAKE_SOURCE_DIR}/src/runtime/system
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/common
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/math
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/2d
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/3d
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/3d/render
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/3d/scene
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/3d/physics
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/3d/anim
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/3d/nav
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/3d/world
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/3d/assets
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/3d/backend
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/gui
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/input
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/media
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/text
            ${CMAKE_SOURCE_DIR}/src/runtime/audio
            ${CMAKE_SOURCE_DIR}/src/runtime/threads
            ${CMAKE_SOURCE_DIR}/src/runtime/network)
    target_link_libraries(${target} PRIVATE zanna_common_opts)
    if (NOT WIN32)
        target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200809L)
        if (NOT APPLE)
            target_compile_definitions(${target} PRIVATE _DEFAULT_SOURCE)
        endif ()
    endif ()
    if (UNIX AND NOT APPLE)
        target_link_libraries(${target} PRIVATE m)
    endif ()
    zanna_add_ctest(${target} $<TARGET_FILE:${target}>)
    zanna_add_test_labels(${target} runtime contract isolated)
    add_dependencies(runtime_contract_tests ${target})
endfunction()

function(zanna_add_unit_support_tests)
    if (TARGET test_support)
        return()
    endif ()

    zanna_add_test(test_support ${ZANNA_TESTS_DIR}/unit/test_support.cpp)
    target_link_libraries(test_support PRIVATE ${ZANNA_UNIT_SUPPORT_LIBS})
    zanna_add_ctest(test_support test_support)

    zanna_add_test(test_diag_catalog ${ZANNA_TESTS_DIR}/unit/test_diag_catalog.cpp)
    target_link_libraries(test_diag_catalog PRIVATE ${ZANNA_UNIT_SUPPORT_LIBS})
    zanna_add_ctest(test_diag_catalog test_diag_catalog)

    zanna_add_test(test_basic_diag_coverage ${ZANNA_TESTS_DIR}/unit/test_basic_diag_coverage.cpp)
    target_link_libraries(test_basic_diag_coverage PRIVATE ${ZANNA_UNIT_SUPPORT_LIBS})
    target_compile_definitions(test_basic_diag_coverage PRIVATE
            ZANNA_BASIC_FRONTEND_DIR="${CMAKE_SOURCE_DIR}/src/frontends/basic")
    zanna_add_ctest(test_basic_diag_coverage test_basic_diag_coverage)
endfunction()

if (NOT TARGET test_basic_terminal_scan)
    zanna_add_test(test_basic_terminal_scan ${ZANNA_TESTS_DIR}/unit/BasicTerminalScanTest.cpp)
    target_link_libraries(test_basic_terminal_scan PRIVATE fe_basic)
    zanna_add_ctest(test_basic_terminal_scan test_basic_terminal_scan)
endif ()

if (NOT TARGET test_frontend_common)
    zanna_add_test(test_frontend_common ${ZANNA_TESTS_DIR}/unit/test_frontend_common.cpp)
    target_link_libraries(test_frontend_common PRIVATE fe_common zanna_il_core)
    zanna_add_ctest(test_frontend_common test_frontend_common)
    zanna_add_test_labels(test_frontend_common unit)
endif ()

if (WIN32 AND NOT TARGET test_windows_installer_version)
    zanna_add_test(test_windows_installer_version
            ${ZANNA_TESTS_DIR}/unit/test_windows_installer_version.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/windows_installer/WindowsInstallerVersion.cpp)
    target_include_directories(test_windows_installer_version PRIVATE
            ${CMAKE_SOURCE_DIR}/src/tools/common/packaging)
    zanna_add_ctest(test_windows_installer_version test_windows_installer_version)
    zanna_add_test_labels(test_windows_installer_version installer packaging requires_windows)
endif ()

if (WIN32 AND NOT TARGET test_windows_installer_update)
    zanna_add_test(test_windows_installer_update
            ${ZANNA_TESTS_DIR}/unit/test_windows_installer_update.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/windows_installer/WindowsInstallerUpdate.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/windows_installer/WindowsInstallerVersion.cpp)
    target_include_directories(test_windows_installer_update PRIVATE
            ${CMAKE_SOURCE_DIR}/src/tools/common/packaging)
    target_link_libraries(test_windows_installer_update PRIVATE
            zanna_packaging bcrypt comctl32 shell32 user32 winhttp)
    # Debug linking retains the uncalled TaskDialogIndirect UI path. Without a
    # v6 activation context Windows loads comctl32 v5 and fails before main.
    target_link_options(test_windows_installer_update PRIVATE
            "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")
    zanna_add_ctest(test_windows_installer_update test_windows_installer_update)
    set_tests_properties(test_windows_installer_update PROPERTIES TIMEOUT 15)
    zanna_add_test_labels(test_windows_installer_update installer packaging requires_windows)
endif ()

if (WIN32 AND NOT TARGET test_windows_installer_lifecycle_contract)
    zanna_add_test(test_windows_installer_lifecycle_contract
            ${ZANNA_TESTS_DIR}/unit/test_windows_installer_lifecycle_contract.cpp)
    target_compile_definitions(test_windows_installer_lifecycle_contract PRIVATE
            ZANNA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    zanna_add_ctest(test_windows_installer_lifecycle_contract
            test_windows_installer_lifecycle_contract)
    zanna_add_test_labels(test_windows_installer_lifecycle_contract
            installer packaging requires_windows unit)
endif ()

if (WIN32 AND NOT TARGET test_windows_installer_brand_validation)
    zanna_add_test(test_windows_installer_brand_validation
            ${ZANNA_TESTS_DIR}/unit/test_windows_installer_brand_validation.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/windows_installer/WindowsInstallerBrandValidation.cpp)
    target_include_directories(test_windows_installer_brand_validation PRIVATE
            ${CMAKE_SOURCE_DIR}/src/tools/windows_installer
            ${CMAKE_SOURCE_DIR}/src/tools/common/packaging)
    zanna_add_ctest(test_windows_installer_brand_validation
            test_windows_installer_brand_validation)
    zanna_add_test_labels(test_windows_installer_brand_validation
            installer unit requires_windows)
endif ()

if (WIN32 AND NOT TARGET test_windows_installer_cleanup_policy)
    zanna_add_test(test_windows_installer_cleanup_policy
            ${ZANNA_TESTS_DIR}/unit/test_windows_installer_cleanup_policy.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/windows_installer/WindowsInstallerCleanupPolicy.cpp)
    zanna_add_ctest(test_windows_installer_cleanup_policy
            test_windows_installer_cleanup_policy)
    zanna_add_test_labels(test_windows_installer_cleanup_policy
            installer packaging requires_windows unit)
endif ()

if (WIN32 AND NOT TARGET test_vg_filedialog_platform_win32)
    zanna_add_test(test_vg_filedialog_platform_win32
            ${ZANNA_TESTS_DIR}/unit/test_vg_filedialog_platform_win32.c
            ${CMAKE_SOURCE_DIR}/src/lib/gui/src/widgets/vg_filedialog_platform_win32.c)
    target_include_directories(test_vg_filedialog_platform_win32 PRIVATE
            ${CMAKE_SOURCE_DIR}/src/lib/gui/src/widgets)
    target_compile_definitions(test_vg_filedialog_platform_win32 PRIVATE
            UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    zanna_add_ctest(test_vg_filedialog_platform_win32
            test_vg_filedialog_platform_win32)
    zanna_add_test_labels(test_vg_filedialog_platform_win32
            gui unit requires_windows)
endif ()

# Packaging subsystem unit tests (ZAPS)
if (NOT TARGET test_packaging)
    if (WIN32)
        # Managed Windows environments can block this standalone test binary via Device Guard.
        # Keep the full executable outside the default suite, but register focused
        # Windows packaging slices that avoid Linux verifier cases with
        # path-convention-specific expectations.
        zanna_add_test(test_packaging ${ZANNA_TESTS_DIR}/unit/test_packaging.cpp NO_CTEST)
        foreach (_zaps_filter
                "PE.*"
                "Verify.PE*"
                "Lnk.*"
                "InstallerStubGen.*"
                "InstallerStub.*"
                "WindowsInstallerMetadata.*"
                "WindowsPackageBuilder.*"
                "ToolchainWindowsPackageBuilder.*")
            string(REPLACE "*" "all" _zaps_filter_name "${_zaps_filter}")
            string(REPLACE "." "_" _zaps_filter_name "${_zaps_filter_name}")
            add_test(NAME "test_packaging_${_zaps_filter_name}"
                    COMMAND test_packaging "--filter=${_zaps_filter}")
            set_tests_properties("test_packaging_${_zaps_filter_name}" PROPERTIES SKIP_RETURN_CODE 77)
            zanna_add_test_labels("test_packaging_${_zaps_filter_name}" packaging installer requires_windows)
        endforeach ()
    else ()
        zanna_add_test(test_packaging ${ZANNA_TESTS_DIR}/unit/test_packaging.cpp)
        zanna_add_test_labels(test_packaging slow)
    endif ()
    target_link_libraries(test_packaging PRIVATE zanna_packaging)
endif ()

# NOTE: Tests for verifier relaxations (partial CBr branch-args, str-to-ptr) removed
# because the verifier does not currently support these relaxations.

# Builtin extern resolution tests (resolver + diagnostics)
if (NOT TARGET test_basic_builtin_extern_resolution)
    zanna_add_test(test_basic_builtin_extern_resolution ${ZANNA_TESTS_DIR}/unit/test_basic_builtin_extern_resolution.cpp)
    target_link_libraries(test_basic_builtin_extern_resolution PRIVATE fe_basic)
    zanna_add_ctest(test_basic_builtin_extern_resolution test_basic_builtin_extern_resolution)
endif ()

# Parser single-index array test (guards HIGH-2 fix)
if (NOT TARGET test_basic_parse_array_single_index)
    zanna_add_test(test_basic_parse_array_single_index ${ZANNA_TESTS_DIR}/unit/test_basic_parse_array_single_index.cpp)
    target_link_libraries(test_basic_parse_array_single_index PRIVATE fe_basic)
    zanna_add_ctest(test_basic_parse_array_single_index test_basic_parse_array_single_index)
endif ()

if (NOT TARGET test_basic_soft_identifier_contexts)
    zanna_add_test(test_basic_soft_identifier_contexts ${ZANNA_TESTS_DIR}/unit/test_basic_soft_identifier_contexts.cpp)
    target_link_libraries(test_basic_soft_identifier_contexts PRIVATE fe_basic)
    zanna_add_ctest(test_basic_soft_identifier_contexts test_basic_soft_identifier_contexts)
endif ()

if (NOT TARGET test_basic_control_recovery)
    zanna_add_test(test_basic_control_recovery ${ZANNA_TESTS_DIR}/unit/test_basic_control_recovery.cpp)
    target_link_libraries(test_basic_control_recovery PRIVATE fe_basic)
    zanna_add_ctest(test_basic_control_recovery test_basic_control_recovery)
endif ()

# Builtins ARGC/ARG$/COMMAND$ lowering test
if (NOT TARGET test_basic_builtins_args_cmd)
    zanna_add_test(test_basic_builtins_args_cmd ${ZANNA_TESTS_DIR}/unit/test_basic_builtins_args_cmd.cpp)
    target_link_libraries(test_basic_builtins_args_cmd PRIVATE fe_basic)
    zanna_add_ctest(test_basic_builtins_args_cmd test_basic_builtins_args_cmd)
endif ()

if (NOT TARGET test_basic_builtins_args_cmd_semantics)
    zanna_add_test(test_basic_builtins_args_cmd_semantics ${ZANNA_TESTS_DIR}/unit/test_basic_builtins_args_cmd_semantics.cpp)
    target_link_libraries(test_basic_builtins_args_cmd_semantics PRIVATE fe_basic)
    zanna_add_ctest(test_basic_builtins_args_cmd_semantics test_basic_builtins_args_cmd_semantics)
endif ()

# Runtime StringBuilder bridge tests
if (NOT TARGET test_stringbuilder_bridge)
    zanna_add_test(test_stringbuilder_bridge ${ZANNA_TESTS_DIR}/unit/test_stringbuilder_bridge.c)
    target_link_libraries(test_stringbuilder_bridge PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_stringbuilder_bridge test_stringbuilder_bridge)
endif ()

if (NOT TARGET test_basic_builtins_arg_get_typing)
    zanna_add_test(test_basic_builtins_arg_get_typing ${ZANNA_TESTS_DIR}/unit/test_basic_builtins_arg_get_typing.cpp)
    target_link_libraries(test_basic_builtins_arg_get_typing PRIVATE fe_basic)
    zanna_add_ctest(test_basic_builtins_arg_get_typing test_basic_builtins_arg_get_typing)
endif ()

if (NOT TARGET test_basic_parse_namespace)
    zanna_add_test(test_basic_parse_namespace ${ZANNA_TESTS_DIR}/unit/test_basic_parse_namespace.cpp)
    target_link_libraries(test_basic_parse_namespace PRIVATE fe_basic)
    zanna_add_ctest(test_basic_parse_namespace test_basic_parse_namespace)
endif ()

if (NOT TARGET test_basic_parse_using)
    zanna_add_test(test_basic_parse_using ${ZANNA_TESTS_DIR}/unit/test_basic_parse_using.cpp)
    target_link_libraries(test_basic_parse_using PRIVATE fe_basic)
    zanna_add_ctest(test_basic_parse_using test_basic_parse_using)
endif ()

# Consolidated namespace/using tests (8 files → 1, 75 TEST() cases)
if (NOT TARGET test_basic_namespace_consolidated)
    zanna_add_test(test_basic_namespace_consolidated
            ${ZANNA_TESTS_DIR}/unit/test_basic_namespace_consolidated.cpp)
    target_link_libraries(test_basic_namespace_consolidated PRIVATE fe_basic)
    zanna_add_ctest(test_basic_namespace_consolidated test_basic_namespace_consolidated)
endif ()

if (NOT TARGET test_type_resolver)
    zanna_add_test(test_type_resolver ${ZANNA_TESTS_DIR}/unit/test_type_resolver.cpp)
    target_link_libraries(test_type_resolver PRIVATE fe_basic)
    zanna_add_ctest(test_type_resolver test_type_resolver)
endif ()

## Removed: test_ns_registry_builder, test_using_semantics, test_ns_resolve_pass
## (consolidated into test_basic_namespace_consolidated)

if (NOT TARGET test_lowerer_namespace)
    zanna_add_test(test_lowerer_namespace ${ZANNA_TESTS_DIR}/unit/test_lowerer_namespace.cpp)
    target_link_libraries(test_lowerer_namespace PRIVATE fe_basic)
    zanna_add_ctest(test_lowerer_namespace test_lowerer_namespace)
endif ()

## Removed: test_namespace_diagnostics, test_namespace_integration
## (consolidated into test_basic_namespace_consolidated)

# Built-in external types catalog recognition
if (NOT TARGET test_builtin_external_types)
    zanna_add_test(test_builtin_external_types ${ZANNA_TESTS_DIR}/unit/types/TestBuiltinExternalTypes.cpp)
    target_link_libraries(test_builtin_external_types PRIVATE fe_basic)
    zanna_add_ctest(test_builtin_external_types test_builtin_external_types)
endif ()

## Removed: test_using_compiletime_only (consolidated into test_basic_namespace_consolidated)

if (NOT TARGET test_basic_proc_diagnostics)
    zanna_add_test(test_basic_proc_diagnostics ${ZANNA_TESTS_DIR}/unit/test_basic_proc_diagnostics.cpp)
    target_link_libraries(test_basic_proc_diagnostics PRIVATE fe_basic)
    zanna_add_ctest(test_basic_proc_diagnostics test_basic_proc_diagnostics)
endif ()

if (NOT TARGET test_basic_ns_case_insensitive)
    zanna_add_test(test_basic_ns_case_insensitive ${ZANNA_TESTS_DIR}/unit/test_basic_ns_case_insensitive.cpp)
    target_link_libraries(test_basic_ns_case_insensitive PRIVATE fe_basic)
    zanna_add_ctest(test_basic_ns_case_insensitive test_basic_ns_case_insensitive)
endif ()

if (NOT TARGET test_basic_il_qualified_names)
    zanna_add_test(test_basic_il_qualified_names ${ZANNA_TESTS_DIR}/unit/test_basic_il_qualified_names.cpp)
    target_link_libraries(test_basic_il_qualified_names PRIVATE fe_basic)
    zanna_add_ctest(test_basic_il_qualified_names test_basic_il_qualified_names)
endif ()

if (NOT TARGET test_ident_util)
    zanna_add_test(test_ident_util ${ZANNA_TESTS_DIR}/unit/test_ident_util.cpp)
    target_link_libraries(test_ident_util PRIVATE fe_basic)
    zanna_add_ctest(test_ident_util test_ident_util)
endif ()

# Bug #021 fix: Method call arguments evaluated only once
if (NOT TARGET test_basic_method_call_arg_single_eval)
    zanna_add_test(test_basic_method_call_arg_single_eval ${ZANNA_TESTS_DIR}/unit/test_basic_method_call_arg_single_eval.cpp)
    target_link_libraries(test_basic_method_call_arg_single_eval PRIVATE fe_basic)
    zanna_add_ctest(test_basic_method_call_arg_single_eval test_basic_method_call_arg_single_eval)
endif ()

# Tail-call tests (enable TCO for these targets)
if (NOT TARGET test_vm_tailcall)
    zanna_add_test(test_vm_tailcall ${ZANNA_TESTS_DIR}/unit/VM_TailCallTests.cpp)
    target_link_libraries(test_vm_tailcall PRIVATE il_build ${ZANNA_VM_LIB} ${ZANNA_VM_SUPPORT_LIB} zanna_test_common)
    target_compile_definitions(test_vm_tailcall PRIVATE ZANNA_VM_TAILCALL=1)
    zanna_add_ctest(test_vm_tailcall test_vm_tailcall)
endif ()

# Runner stepping API tests
if (NOT TARGET test_vm_step_api)
    zanna_add_test(test_vm_step_api ${ZANNA_TESTS_DIR}/unit/VM_StepApiTests.cpp)
    # Link directly to il_vm to avoid relying on VM vars ordering
    target_link_libraries(test_vm_step_api PRIVATE il_build il_vm zanna_support)
    zanna_add_ctest(test_vm_step_api test_vm_step_api)
endif ()

if (NOT TARGET test_vm_step_tco)
    zanna_add_test(test_vm_step_tco ${ZANNA_TESTS_DIR}/unit/VM_StepTcoTests.cpp)
    target_link_libraries(test_vm_step_tco PRIVATE il_build il_vm zanna_support)
    target_compile_definitions(test_vm_step_tco PRIVATE ZANNA_VM_TAILCALL=1)
    zanna_add_ctest(test_vm_step_tco test_vm_step_tco)
endif ()

if (NOT TARGET test_vm_tailcall_eh)
    zanna_add_test(test_vm_tailcall_eh ${ZANNA_TESTS_DIR}/unit/VM_TailCallEhTests.cpp)
    target_link_libraries(test_vm_tailcall_eh PRIVATE il_build ${ZANNA_VM_LIB} ${ZANNA_VM_SUPPORT_LIB} zanna_test_common)
    target_compile_definitions(test_vm_tailcall_eh PRIVATE ZANNA_VM_TAILCALL=1)
    zanna_add_ctest(test_vm_tailcall_eh test_vm_tailcall_eh)
endif ()

# Opcode counts tests
if (NOT TARGET test_vm_opcode_counts)
    zanna_add_test(test_vm_opcode_counts ${ZANNA_TESTS_DIR}/unit/VM_OpcodeCountsTests.cpp)
    target_link_libraries(test_vm_opcode_counts PRIVATE il_build il_vm zanna_support)
    zanna_add_ctest(test_vm_opcode_counts test_vm_opcode_counts)
endif ()

# Opcode profiling test
if (NOT TARGET test_vm_opcode_profiling)
    zanna_add_test(test_vm_opcode_profiling ${ZANNA_TESTS_DIR}/unit/test_vm_opcode_profiling.cpp)
    target_link_libraries(test_vm_opcode_profiling PRIVATE zanna_il_full il_vm il_api)
    zanna_add_ctest(test_vm_opcode_profiling test_vm_opcode_profiling)
endif ()

# Runtime class catalog seeding test
if (NOT TARGET test_runtime_class_seeding)
    zanna_add_test(test_runtime_class_seeding ${ZANNA_TESTS_DIR}/unit/test_runtime_class_seeding.cpp)
    target_link_libraries(test_runtime_class_seeding PRIVATE fe_basic)
    zanna_add_ctest(test_runtime_class_seeding test_runtime_class_seeding)
endif ()

# Runtime class catalog smoke test
if (NOT TARGET test_runtime_classes_catalog)
    zanna_add_test(test_runtime_classes_catalog ${ZANNA_TESTS_DIR}/unit/test_runtime_classes_catalog.cpp)
    # Link with frontend to ensure catalog is reachable in front-end contexts
    target_link_libraries(test_runtime_classes_catalog PRIVATE fe_basic)
    zanna_add_ctest(test_runtime_classes_catalog test_runtime_classes_catalog)
endif ()

if (NOT TARGET test_runtime_surface_audit)
    zanna_add_test(test_runtime_surface_audit ${ZANNA_TESTS_DIR}/unit/test_runtime_surface_audit.cpp)
    target_link_libraries(test_runtime_surface_audit PRIVATE fe_basic)
    target_compile_definitions(test_runtime_surface_audit PRIVATE ZANNA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    zanna_add_ctest(test_runtime_surface_audit test_runtime_surface_audit)
endif ()

if (NOT TARGET test_rtgen_header_parse)
    zanna_add_test(test_rtgen_header_parse ${ZANNA_TESTS_DIR}/unit/test_rtgen_header_parse.cpp)
    zanna_add_ctest(test_rtgen_header_parse test_rtgen_header_parse)
endif ()

zanna_add_ctest(test_runtime_surface_cli
        $<TARGET_FILE:rtgen>
        --audit
        --strict-header-sync
        --strict-unclassified
        --summary-only
        ${CMAKE_SOURCE_DIR}/src/il/runtime/runtime.def)

# New unit tests for RC-B7 runtime-class pipeline
if (NOT TARGET test_catalog_basic)
    zanna_add_test(test_catalog_basic ${ZANNA_TESTS_DIR}/unit/runtime_classes/TestCatalog.cpp)
    target_link_libraries(test_catalog_basic PRIVATE fe_basic)
    zanna_add_ctest(test_catalog_basic test_catalog_basic)
endif ()

if (NOT TARGET test_property_index_basic)
    zanna_add_test(test_property_index_basic ${ZANNA_TESTS_DIR}/unit/runtime_classes/TestPropertyIndex.cpp)
    target_link_libraries(test_property_index_basic PRIVATE fe_basic)
    zanna_add_ctest(test_property_index_basic test_property_index_basic)
endif ()

if (NOT TARGET test_method_index_basic)
    zanna_add_test(test_method_index_basic ${ZANNA_TESTS_DIR}/unit/runtime_classes/TestMethodIndex.cpp)
    target_link_libraries(test_method_index_basic PRIVATE fe_basic)
    zanna_add_ctest(test_method_index_basic test_method_index_basic)
endif ()

if (NOT TARGET test_runtime_class_typed_returns)
    zanna_add_test(test_runtime_class_typed_returns
            ${ZANNA_TESTS_DIR}/unit/runtime_classes/TestTypedReturnMetadata.cpp)
    target_link_libraries(test_runtime_class_typed_returns PRIVATE fe_basic)
    zanna_add_ctest(test_runtime_class_typed_returns test_runtime_class_typed_returns)
endif ()

if (NOT TARGET test_lowering_runtime_class_len)
    zanna_add_test(test_lowering_runtime_class_len ${ZANNA_TESTS_DIR}/unit/runtime_classes/TestLowering.cpp)
    target_link_libraries(test_lowering_runtime_class_len PRIVATE fe_basic)
    zanna_add_ctest(test_lowering_runtime_class_len test_lowering_runtime_class_len)
endif ()

# System.Text.StringBuilder binding tests (ctor and Capacity getter)
if (NOT TARGET test_stringbuilder_binding)
    zanna_add_test(test_stringbuilder_binding ${ZANNA_TESTS_DIR}/unit/runtime_classes/TestStringBuilderBinding.cpp)
    target_link_libraries(test_stringbuilder_binding PRIVATE fe_basic)
    zanna_add_ctest(test_stringbuilder_binding test_stringbuilder_binding)
endif ()

# System.IO.File binding tests
if (NOT TARGET test_runtime_class_file_binding)
    zanna_add_test(test_runtime_class_file_binding ${ZANNA_TESTS_DIR}/unit/runtime_classes/TestRuntimeClassFileBinding.cpp)
    target_link_libraries(test_runtime_class_file_binding PRIVATE fe_basic)
    zanna_add_ctest(test_runtime_class_file_binding test_runtime_class_file_binding)
endif ()

# System.Collections.List binding tests
if (NOT TARGET test_runtime_class_list_binding)
    zanna_add_test(test_runtime_class_list_binding ${ZANNA_TESTS_DIR}/unit/runtime_classes/TestRuntimeClassListBinding.cpp)
    target_link_libraries(test_runtime_class_list_binding PRIVATE fe_basic)
    zanna_add_ctest(test_runtime_class_list_binding test_runtime_class_list_binding)
endif ()

if (NOT TARGET test_http_server_binding)
    zanna_add_test(test_http_server_binding ${ZANNA_TESTS_DIR}/unit/runtime_classes/TestHttpServerBinding.cpp)
    target_link_libraries(test_http_server_binding PRIVATE fe_basic)
    zanna_add_ctest(test_http_server_binding test_http_server_binding)
endif ()

# Zanna.Console binding tests
if (NOT TARGET test_console_binding)
    zanna_add_test(test_console_binding ${ZANNA_TESTS_DIR}/unit/runtime_classes/TestConsoleBinding.cpp)
    target_link_libraries(test_console_binding PRIVATE fe_basic)
    zanna_add_ctest(test_console_binding test_console_binding)
endif ()

# Zanna.Core.Convert binding tests
if (NOT TARGET test_convert_binding)
    zanna_add_test(test_convert_binding ${ZANNA_TESTS_DIR}/unit/runtime_classes/TestConvertBinding.cpp)
    target_link_libraries(test_convert_binding PRIVATE fe_basic)
    zanna_add_ctest(test_convert_binding test_convert_binding)
endif ()

if (NOT TARGET test_runtime_class_tostring_user)
    zanna_add_test(test_runtime_class_tostring_user ${ZANNA_TESTS_DIR}/unit/runtime_classes/TestRuntimeClassToString.cpp)
    target_link_libraries(test_runtime_class_tostring_user PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_runtime_class_tostring_user test_runtime_class_tostring_user)
endif ()


# Poll callback tests
if (NOT TARGET test_vm_poll_callback)
    zanna_add_test(test_vm_poll_callback ${ZANNA_TESTS_DIR}/unit/VM_PollCallbackTests.cpp)
    target_link_libraries(test_vm_poll_callback PRIVATE il_build il_vm zanna_support)
    zanna_add_ctest(test_vm_poll_callback test_vm_poll_callback)
endif ()

# Dispatch strategy equivalence test
if (NOT TARGET test_vm_dispatch_equivalence)
    zanna_add_test(test_vm_dispatch_equivalence ${ZANNA_TESTS_DIR}/unit/test_vm_dispatch_equivalence.cpp)
    target_link_libraries(test_vm_dispatch_equivalence PRIVATE il_build il_vm zanna_support)
    zanna_add_ctest(test_vm_dispatch_equivalence test_vm_dispatch_equivalence)
endif ()

# Extern registry tests
if (NOT TARGET test_vm_extern_registry)
    zanna_add_test(test_vm_extern_registry ${ZANNA_TESTS_DIR}/unit/VM_ExternRegistryTests.cpp)
    target_link_libraries(test_vm_extern_registry PRIVATE il_build il_vm zanna_support zanna_test_common)
    zanna_add_ctest(test_vm_extern_registry test_vm_extern_registry)
endif ()

# Memory watch tests
if (NOT TARGET test_vm_mem_watch)
    zanna_add_test(test_vm_mem_watch ${ZANNA_TESTS_DIR}/unit/VM_MemWatchTests.cpp)
    target_link_libraries(test_vm_mem_watch PRIVATE il_build il_vm zanna_support)
    zanna_add_ctest(test_vm_mem_watch test_vm_mem_watch)
endif ()

if (NOT TARGET test_integer_helpers)
    zanna_add_test(test_integer_helpers ${ZANNA_TESTS_DIR}/unit/test_integer_helpers.cpp)
    target_link_libraries(test_integer_helpers PRIVATE ${ZANNA_UNIT_SUPPORT_LIBS})
    zanna_add_ctest(test_integer_helpers test_integer_helpers)
endif ()

if (NOT TARGET test_mangle)
    zanna_add_test(test_mangle
            ${ZANNA_TESTS_DIR}/unit/test_mangle.cpp
            ${CMAKE_SOURCE_DIR}/src/common/Mangle.cpp)
    zanna_add_ctest(test_mangle test_mangle)
endif ()

if (NOT TARGET test_il_verify_all)
    zanna_add_test(test_il_verify_all ${ZANNA_TESTS_DIR}/unit/test_il_verify_all.cpp)
    target_link_libraries(test_il_verify_all PRIVATE zanna_il_full)
    zanna_add_ctest(test_il_verify_all test_il_verify_all)
endif ()

if (NOT TARGET test_codegen_x86_64_passes)
    zanna_add_test(
            test_codegen_x86_64_passes
            ${ZANNA_TESTS_DIR}/unit/test_codegen_x86_64_passes.cpp)
    target_link_libraries(test_codegen_x86_64_passes PRIVATE zanna_codegen_x86_64)
    zanna_add_ctest(test_codegen_x86_64_passes test_codegen_x86_64_passes)
endif ()

if (NOT TARGET test_native_eh_lowering)
    zanna_add_test(
            test_native_eh_lowering
            ${ZANNA_TESTS_DIR}/unit/test_native_eh_lowering.cpp)
    target_link_libraries(test_native_eh_lowering PRIVATE zanna_codegen_common il_api)
    zanna_add_ctest(test_native_eh_lowering test_native_eh_lowering)
endif ()

if (NOT TARGET test_codegen_x86_64_live_intervals)
    zanna_add_test(
            test_codegen_x86_64_live_intervals
            ${ZANNA_TESTS_DIR}/unit/test_codegen_x86_64_live_intervals.cpp)
    target_link_libraries(test_codegen_x86_64_live_intervals PRIVATE zanna_codegen_x86_64)
    zanna_add_ctest(test_codegen_x86_64_live_intervals test_codegen_x86_64_live_intervals)
endif ()

if (NOT TARGET test_codegen_x86_64_spiller)
    zanna_add_test(
            test_codegen_x86_64_spiller
            ${ZANNA_TESTS_DIR}/unit/test_codegen_x86_64_spiller.cpp)
    target_link_libraries(test_codegen_x86_64_spiller PRIVATE zanna_codegen_x86_64)
    zanna_add_ctest(test_codegen_x86_64_spiller test_codegen_x86_64_spiller)
endif ()

if (NOT TARGET test_codegen_x86_64_allocator)
    zanna_add_test(
            test_codegen_x86_64_allocator
            ${ZANNA_TESTS_DIR}/unit/test_codegen_x86_64_allocator.cpp)
    target_link_libraries(test_codegen_x86_64_allocator PRIVATE zanna_codegen_x86_64)
    zanna_add_ctest(test_codegen_x86_64_allocator test_codegen_x86_64_allocator)
endif ()

if (NOT TARGET test_codegen_x86_64_coalescer)
    zanna_add_test(
            test_codegen_x86_64_coalescer
            ${ZANNA_TESTS_DIR}/unit/test_codegen_x86_64_coalescer.cpp)
    target_link_libraries(test_codegen_x86_64_coalescer PRIVATE zanna_codegen_x86_64)
    zanna_add_ctest(test_codegen_x86_64_coalescer test_codegen_x86_64_coalescer)
endif ()

if (NOT TARGET test_codegen_x86_64_lowering_rules)
    zanna_add_test(
            test_codegen_x86_64_lowering_rules
            ${ZANNA_TESTS_DIR}/unit/test_codegen_x86_64_lowering_rules.cpp)
    target_link_libraries(test_codegen_x86_64_lowering_rules PRIVATE zanna_codegen_x86_64)
    zanna_add_ctest(test_codegen_x86_64_lowering_rules test_codegen_x86_64_lowering_rules)
endif ()

if (NOT TARGET test_x86_call_abi)
    zanna_add_test(
            test_x86_call_abi
            ${ZANNA_TESTS_DIR}/unit/codegen/test_x86_call_abi.cpp)
    target_link_libraries(test_x86_call_abi PRIVATE zanna_codegen_x86_64)
    zanna_add_ctest(test_x86_call_abi test_x86_call_abi)
endif ()

if (NOT TARGET test_x86_backend_regressions)
    zanna_add_test(
            test_x86_backend_regressions
            ${ZANNA_TESTS_DIR}/unit/codegen/test_x86_backend_regressions.cpp)
    target_link_libraries(test_x86_backend_regressions PRIVATE zanna_codegen_x86_64)
    zanna_add_ctest(test_x86_backend_regressions test_x86_backend_regressions)
endif ()

if (NOT TARGET test_x86_global_ra)
    zanna_add_test(
            test_x86_global_ra
            ${ZANNA_TESTS_DIR}/unit/codegen/test_x86_global_ra.cpp)
    target_link_libraries(test_x86_global_ra PRIVATE zanna_codegen_x86_64)
    zanna_add_ctest(test_x86_global_ra test_x86_global_ra)
endif ()

if (NOT TARGET test_magic_division)
    zanna_add_test(
            test_magic_division
            ${ZANNA_TESTS_DIR}/unit/codegen/test_magic_division.cpp)
    target_link_libraries(test_magic_division PRIVATE zanna_codegen_x86_64)
    zanna_add_ctest(test_magic_division test_magic_division)
endif ()

if (NOT TARGET test_codegen_review_x64_frame)
    zanna_add_test(
            test_codegen_review_x64_frame
            ${ZANNA_TESTS_DIR}/unit/codegen/test_codegen_review_x64_frame.cpp)
    target_link_libraries(test_codegen_review_x64_frame PRIVATE zanna_codegen_x86_64)
    zanna_add_ctest(test_codegen_review_x64_frame test_codegen_review_x64_frame)
endif ()

if (NOT TARGET test_codegen_review_fixes_batch2)
    zanna_add_test(
            test_codegen_review_fixes_batch2
            ${ZANNA_TESTS_DIR}/unit/codegen/test_codegen_review_fixes_batch2.cpp)
    target_link_libraries(test_codegen_review_fixes_batch2 PRIVATE zanna_codegen_x86_64)
    zanna_add_ctest(test_codegen_review_fixes_batch2 test_codegen_review_fixes_batch2)
endif ()

if (NOT TARGET test_codegen_review_fixes_batch3)
    zanna_add_test(
            test_codegen_review_fixes_batch3
            ${ZANNA_TESTS_DIR}/unit/codegen/test_codegen_review_fixes_batch3.cpp)
    target_link_libraries(test_codegen_review_fixes_batch3 PRIVATE zanna_codegen_x86_64 il_codegen_aarch64)
    zanna_add_ctest(test_codegen_review_fixes_batch3 test_codegen_review_fixes_batch3)
endif ()

if (NOT TARGET test_codegen_preregalloc_opt)
    zanna_add_test(
            test_codegen_preregalloc_opt
            ${ZANNA_TESTS_DIR}/unit/codegen/test_codegen_preregalloc_opt.cpp)
    target_link_libraries(test_codegen_preregalloc_opt PRIVATE zanna_codegen_x86_64 il_codegen_aarch64)
    zanna_add_ctest(test_codegen_preregalloc_opt test_codegen_preregalloc_opt)
endif ()

if (NOT TARGET test_codegen_peephole_optimizations)
    zanna_add_test(
            test_codegen_peephole_optimizations
            ${ZANNA_TESTS_DIR}/unit/codegen/test_codegen_peephole_optimizations.cpp)
    target_link_libraries(test_codegen_peephole_optimizations PRIVATE il_codegen_aarch64)
    zanna_add_ctest(test_codegen_peephole_optimizations test_codegen_peephole_optimizations)
endif ()

if (NOT TARGET test_x86_isel_lea_mul)
    zanna_add_test(
            test_x86_isel_lea_mul
            ${ZANNA_TESTS_DIR}/unit/codegen/test_x86_isel_lea_mul.cpp)
    target_link_libraries(test_x86_isel_lea_mul PRIVATE zanna_codegen_x86_64)
    zanna_add_ctest(test_x86_isel_lea_mul test_x86_isel_lea_mul)
endif ()

if (NOT TARGET test_x86_peephole)
    zanna_add_test(
            test_x86_peephole
            ${ZANNA_TESTS_DIR}/unit/codegen/test_x86_peephole.cpp)
    target_link_libraries(test_x86_peephole PRIVATE zanna_codegen_x86_64)
    zanna_add_ctest(test_x86_peephole test_x86_peephole)
endif ()

if (NOT TARGET test_dataflow_liveness)
    zanna_add_test(
            test_dataflow_liveness
            ${ZANNA_TESTS_DIR}/unit/codegen/test_dataflow_liveness.cpp)
    target_link_libraries(test_dataflow_liveness PRIVATE zanna_codegen_common)
    zanna_add_ctest(test_dataflow_liveness test_dataflow_liveness)
endif ()

if (NOT TARGET test_codegen_cfg_extract)
    zanna_add_test(
            test_codegen_cfg_extract
            ${ZANNA_TESTS_DIR}/unit/codegen/test_codegen_cfg_extract.cpp)
    target_link_libraries(test_codegen_cfg_extract PRIVATE zanna_codegen_x86_64 il_codegen_aarch64)
    zanna_add_ctest(test_codegen_cfg_extract test_codegen_cfg_extract)
endif ()

if (NOT TARGET test_aarch64_pac_bti)
    zanna_add_test(
            test_aarch64_pac_bti
            ${ZANNA_TESTS_DIR}/unit/codegen/test_aarch64_pac_bti.cpp)
    target_link_libraries(test_aarch64_pac_bti PRIVATE il_codegen_aarch64)
    zanna_add_ctest(test_aarch64_pac_bti test_aarch64_pac_bti)
endif ()

if (NOT TARGET test_shared_frame_layout)
    zanna_add_test(
            test_shared_frame_layout
            ${ZANNA_TESTS_DIR}/unit/codegen/test_shared_frame_layout.cpp)
    target_link_libraries(test_shared_frame_layout PRIVATE il_codegen_aarch64)
    zanna_add_ctest(test_shared_frame_layout test_shared_frame_layout)
endif ()

if (NOT TARGET test_ra_victim_selection)
    zanna_add_test(
            test_ra_victim_selection
            ${ZANNA_TESTS_DIR}/unit/codegen/test_ra_victim_selection.cpp)
    target_link_libraries(test_ra_victim_selection PRIVATE zanna_codegen_common)
    zanna_add_ctest(test_ra_victim_selection test_ra_victim_selection)
endif ()

if (NOT TARGET test_aarch64_opcode_def)
    zanna_add_test(
            test_aarch64_opcode_def
            ${ZANNA_TESTS_DIR}/unit/codegen/test_aarch64_opcode_def.cpp)
    target_link_libraries(test_aarch64_opcode_def PRIVATE il_codegen_aarch64)
    zanna_add_ctest(test_aarch64_opcode_def test_aarch64_opcode_def)
endif ()

if (NOT TARGET test_aarch64_vararg)
    zanna_add_test(
            test_aarch64_vararg
            ${ZANNA_TESTS_DIR}/unit/codegen/test_aarch64_vararg.cpp)
    target_link_libraries(test_aarch64_vararg PRIVATE il_codegen_aarch64 il_runtime)
    zanna_add_ctest(test_aarch64_vararg test_aarch64_vararg)
endif ()

if (NOT TARGET test_aarch64_fp_compare_lowering)
    zanna_add_test(
            test_aarch64_fp_compare_lowering
            ${ZANNA_TESTS_DIR}/unit/codegen/test_aarch64_fp_compare_lowering.cpp)
    target_link_libraries(test_aarch64_fp_compare_lowering PRIVATE il_codegen_aarch64)
    zanna_add_ctest(test_aarch64_fp_compare_lowering test_aarch64_fp_compare_lowering)
endif ()

if (NOT TARGET test_aarch64_trap_blocks)
    zanna_add_test(
            test_aarch64_trap_blocks
            ${ZANNA_TESTS_DIR}/unit/codegen/test_aarch64_trap_blocks.cpp)
    target_link_libraries(test_aarch64_trap_blocks PRIVATE il_codegen_aarch64)
    zanna_add_ctest(test_aarch64_trap_blocks test_aarch64_trap_blocks)
endif ()

if (NOT TARGET test_aarch64_cross_block_reload)
    zanna_add_test(
            test_aarch64_cross_block_reload
            ${ZANNA_TESTS_DIR}/unit/codegen/test_aarch64_cross_block_reload.cpp)
    target_link_libraries(test_aarch64_cross_block_reload PRIVATE il_codegen_aarch64)
    zanna_add_ctest(test_aarch64_cross_block_reload test_aarch64_cross_block_reload)
endif ()

# Bytecode VM tests
if (NOT TARGET test_bytecode_vm)
    zanna_add_test(test_bytecode_vm ${ZANNA_TESTS_DIR}/unit/test_bytecode_vm.cpp)
    target_link_libraries(test_bytecode_vm PRIVATE zanna_bytecode il_build)
    zanna_add_ctest(test_bytecode_vm test_bytecode_vm)
endif ()

if (NOT TARGET test_bytecode_compiler_diagnostics)
    zanna_add_test(test_bytecode_compiler_diagnostics
            ${ZANNA_TESTS_DIR}/unit/test_bytecode_compiler_diagnostics.cpp)
    target_link_libraries(test_bytecode_compiler_diagnostics PRIVATE zanna_bytecode)
    zanna_add_ctest(test_bytecode_compiler_diagnostics test_bytecode_compiler_diagnostics)
endif ()

if (NOT TARGET test_vm_executor_compile_diagnostics)
    zanna_add_test(test_vm_executor_compile_diagnostics
            ${ZANNA_TESTS_DIR}/unit/test_vm_executor_compile_diagnostics.cpp)
    target_link_libraries(test_vm_executor_compile_diagnostics PRIVATE il_tools_common)
    zanna_add_ctest(test_vm_executor_compile_diagnostics test_vm_executor_compile_diagnostics)
endif ()

if (NOT TARGET test_vm_init_diagnostics)
    zanna_add_test(test_vm_init_diagnostics ${ZANNA_TESTS_DIR}/unit/test_vm_init_diagnostics.cpp)
    target_link_libraries(test_vm_init_diagnostics PRIVATE il_vm)
    zanna_add_ctest(test_vm_init_diagnostics test_vm_init_diagnostics)
endif ()

# VM vs BytecodeVM equivalence tests
if (NOT TARGET test_vm_bytecode_equivalence)
    zanna_add_test(test_vm_bytecode_equivalence ${ZANNA_TESTS_DIR}/unit/test_vm_bytecode_equivalence.cpp)
    target_link_libraries(test_vm_bytecode_equivalence PRIVATE il_build il_vm zanna_bytecode zanna_support)
    zanna_add_ctest(test_vm_bytecode_equivalence test_vm_bytecode_equivalence)
endif ()

function(zanna_add_runtime_tests)
    if (TARGET test_rt_string)
        return()
    endif ()

    zanna_add_test(test_rt_string ${ZANNA_TESTS_DIR}/unit/test_rt_string.cpp)
    target_link_libraries(test_rt_string PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_string test_rt_string)

    zanna_add_test(test_rt_string_sso ${ZANNA_TESTS_DIR}/unit/test_rt_string_sso.cpp)
    target_link_libraries(test_rt_string_sso PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_string_sso test_rt_string_sso)

    zanna_add_test(test_rt_string_invalid ${ZANNA_TESTS_DIR}/unit/test_rt_string_invalid.cpp)
    target_link_libraries(test_rt_string_invalid PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_string_invalid test_rt_string_invalid)

    zanna_add_test(test_rt_conv ${ZANNA_TESTS_DIR}/unit/test_rt_conv.cpp)
    target_link_libraries(test_rt_conv PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_conv test_rt_conv)

    zanna_add_test(test_rt_int_to_str_big ${ZANNA_TESTS_DIR}/unit/test_rt_int_to_str_big.cpp)
    target_link_libraries(test_rt_int_to_str_big PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_int_to_str_big test_rt_int_to_str_big)

    zanna_add_test(test_rt_val_str ${ZANNA_TESTS_DIR}/runtime/RTValStrTests.cpp)
    target_link_libraries(test_rt_val_str PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_val_str test_rt_val_str)

    zanna_add_test(test_rt_bitmapfont ${ZANNA_TESTS_DIR}/unit/test_rt_bitmapfont.cpp)
    target_link_libraries(test_rt_bitmapfont PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_bitmapfont test_rt_bitmapfont)

    zanna_add_test(test_rt_gameui ${ZANNA_TESTS_DIR}/unit/test_rt_gameui.cpp)
    target_link_libraries(test_rt_gameui PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_gameui test_rt_gameui)

    zanna_add_test(test_rt_pathfinder ${ZANNA_TESTS_DIR}/unit/test_rt_pathfinder.cpp)
    target_link_libraries(test_rt_pathfinder PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_pathfinder test_rt_pathfinder)

    zanna_add_test(test_rt_screenfx_transitions ${ZANNA_TESTS_DIR}/unit/test_rt_screenfx_transitions.cpp)
    target_link_libraries(test_rt_screenfx_transitions PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_screenfx_transitions test_rt_screenfx_transitions)

    zanna_add_test(test_rt_audio_mixing ${ZANNA_TESTS_DIR}/unit/test_rt_audio_mixing.cpp)
    target_link_libraries(test_rt_audio_mixing PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads)
    zanna_add_ctest(test_rt_audio_mixing test_rt_audio_mixing)

    zanna_add_test(test_rt_runtime_additions ${ZANNA_TESTS_DIR}/unit/test_rt_runtime_additions.cpp)
    target_link_libraries(test_rt_runtime_additions PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_runtime_additions test_rt_runtime_additions)

    zanna_add_test(test_rt_tilemap_layers ${ZANNA_TESTS_DIR}/unit/test_rt_tilemap_layers.cpp)
    target_link_libraries(test_rt_tilemap_layers PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_tilemap_layers test_rt_tilemap_layers)

    zanna_add_isolated_runtime_test(test_rt_tilemap_render_contract
            ${ZANNA_TESTS_DIR}/runtime/RTTilemapRenderContractTests.cpp
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/2d/rt_tilemap.c)
    target_compile_definitions(test_rt_tilemap_render_contract PRIVATE
            RT_PIXELS_INTERNAL_ASSUME_STRUCT_HANDLE=1)

    zanna_add_test(test_rt_physics_joints ${ZANNA_TESTS_DIR}/unit/test_rt_physics_joints.cpp)
    target_link_libraries(test_rt_physics_joints PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_physics_joints test_rt_physics_joints)

    zanna_add_test(test_rt_dialogue ${ZANNA_TESTS_DIR}/unit/test_rt_dialogue.cpp)
    target_link_libraries(test_rt_dialogue PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_dialogue test_rt_dialogue)

    zanna_add_isolated_runtime_test(test_rt_dialogue_contract
            ${ZANNA_TESTS_DIR}/runtime/RTDialogueContractTests.cpp
            ${CMAKE_SOURCE_DIR}/src/runtime/game/rt_dialogue.c)

    zanna_add_test(test_rt_zstd ${ZANNA_TESTS_DIR}/unit/test_rt_zstd.cpp)
    target_link_libraries(test_rt_zstd PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    target_compile_definitions(test_rt_zstd PRIVATE ZANNA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    zanna_add_ctest(test_rt_zstd test_rt_zstd)
    zanna_add_test_labels(test_rt_zstd runtime unit)

    zanna_add_test(test_rt_tilemap_io ${ZANNA_TESTS_DIR}/unit/test_rt_tilemap_io.cpp)
    target_link_libraries(test_rt_tilemap_io PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_tilemap_io test_rt_tilemap_io)

    zanna_add_test(test_rt_canvas3d ${ZANNA_TESTS_DIR}/unit/test_rt_canvas3d.cpp)
    target_link_libraries(test_rt_canvas3d PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    target_compile_definitions(test_rt_canvas3d PRIVATE ZANNA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    zanna_add_ctest(test_rt_canvas3d test_rt_canvas3d)
    zanna_add_test_labels(test_rt_canvas3d graphics3d unit)
    set_tests_properties(test_rt_canvas3d PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

    if (UNIX AND NOT APPLE AND (ZANNAGFX_BACKEND STREQUAL "Wayland" OR
                               ZANNAGFX_BACKEND STREQUAL "LinuxAuto"))
        zanna_add_test(test_wayland_egl ${ZANNA_TESTS_DIR}/unit/test_wayland_egl.cpp)
        target_link_libraries(test_wayland_egl PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
        target_include_directories(test_wayland_egl PRIVATE
                ${CMAKE_SOURCE_DIR}/src/runtime/graphics/3d/backend)
        zanna_add_ctest(test_wayland_egl test_wayland_egl)
        zanna_add_test_labels(test_wayland_egl graphics3d e2e gui requires_display requires_linux)
        set_tests_properties(test_wayland_egl PROPERTIES TIMEOUT 20)
    endif ()

    zanna_add_test(test_rt_mesh_simplify ${ZANNA_TESTS_DIR}/unit/test_rt_mesh_simplify.cpp)
    target_link_libraries(test_rt_mesh_simplify PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_mesh_simplify test_rt_mesh_simplify)
    zanna_add_test_labels(test_rt_mesh_simplify graphics3d unit contract)

    zanna_add_test(test_quickhull3d ${ZANNA_TESTS_DIR}/unit/test_quickhull3d.cpp)
    target_link_libraries(test_quickhull3d PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    target_include_directories(test_quickhull3d PRIVATE ${CMAKE_SOURCE_DIR}/src/runtime/graphics/3d/physics)
    zanna_add_ctest(test_quickhull3d test_quickhull3d)
    zanna_add_test_labels(test_quickhull3d graphics3d unit)

    zanna_add_test(test_rt_cubemap3d_ibl ${ZANNA_TESTS_DIR}/unit/test_rt_cubemap3d_ibl.cpp)
    target_link_libraries(test_rt_cubemap3d_ibl PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_cubemap3d_ibl test_rt_cubemap3d_ibl)
    zanna_add_test_labels(test_rt_cubemap3d_ibl graphics3d unit)

    zanna_add_test(test_graphics3d_abi_surface ${ZANNA_TESTS_DIR}/unit/test_graphics3d_abi_surface.cpp)
    target_compile_definitions(test_graphics3d_abi_surface PRIVATE ZANNA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    zanna_add_ctest(test_graphics3d_abi_surface test_graphics3d_abi_surface)
    zanna_add_test_labels(test_graphics3d_abi_surface graphics3d unit contract)

    zanna_add_test(test_graphics3d_runtime_manifest
            ${ZANNA_TESTS_DIR}/unit/test_graphics3d_runtime_manifest.cpp)
    target_link_libraries(test_graphics3d_runtime_manifest PRIVATE fe_basic)
    zanna_add_ctest(test_graphics3d_runtime_manifest test_graphics3d_runtime_manifest)
    zanna_add_test_labels(test_graphics3d_runtime_manifest graphics3d unit contract)

    zanna_add_test(test_gui_runtime_manifest
            ${ZANNA_TESTS_DIR}/unit/test_gui_runtime_manifest.cpp)
    target_link_libraries(test_gui_runtime_manifest PRIVATE fe_basic)
    zanna_add_ctest(test_gui_runtime_manifest test_gui_runtime_manifest)
    zanna_add_test_labels(test_gui_runtime_manifest gui unit contract)

    zanna_add_test(test_runtime_class_qualified_surface
            ${ZANNA_TESTS_DIR}/unit/test_runtime_class_qualified_surface.cpp)
    target_include_directories(test_runtime_class_qualified_surface PRIVATE ${CMAKE_SOURCE_DIR}/src)
    zanna_add_ctest(test_runtime_class_qualified_surface test_runtime_class_qualified_surface)
    zanna_add_test_labels(test_runtime_class_qualified_surface graphics3d unit contract)

    zanna_add_test(test_rt_canvas3d_production ${ZANNA_TESTS_DIR}/unit/test_rt_canvas3d_production.cpp)
    target_link_libraries(test_rt_canvas3d_production PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    target_compile_definitions(test_rt_canvas3d_production PRIVATE RT_G3D_ALLOW_STACK_FIXTURES=1)
    zanna_add_ctest(test_rt_canvas3d_production test_rt_canvas3d_production)
    zanna_add_test_labels(test_rt_canvas3d_production graphics3d unit)

    zanna_add_test(test_rt_material3d ${ZANNA_TESTS_DIR}/unit/test_rt_material3d.cpp)
    target_link_libraries(test_rt_material3d PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_material3d test_rt_material3d)
    zanna_add_test_labels(test_rt_material3d graphics3d unit)

    # The core VSCN/scene suite runs in the default gate; only the ~30s
    # 10k spatial-index scaling fixture stays behind the slow label, run
    # from the same binary via --scale.
    zanna_add_test(test_rt_scene3d ${ZANNA_TESTS_DIR}/unit/test_rt_scene3d.cpp)
    target_link_libraries(test_rt_scene3d PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_scene3d test_rt_scene3d)
    zanna_add_test_labels(test_rt_scene3d graphics3d unit)
    zanna_add_ctest(test_rt_scene3d_scale test_rt_scene3d --scale)
    zanna_add_test_labels(test_rt_scene3d_scale graphics3d unit slow)

    zanna_add_test(test_rt_scene3d_bindings ${ZANNA_TESTS_DIR}/unit/test_rt_scene3d_bindings.cpp)
    target_link_libraries(test_rt_scene3d_bindings PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_scene3d_bindings test_rt_scene3d_bindings)
    zanna_add_test_labels(test_rt_scene3d_bindings graphics3d unit)

    zanna_add_test(test_rt_cloth3d ${ZANNA_TESTS_DIR}/unit/test_rt_cloth3d.cpp)
    target_link_libraries(test_rt_cloth3d PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_cloth3d test_rt_cloth3d)
    zanna_add_test_labels(test_rt_cloth3d graphics3d unit)

    zanna_add_test(test_rt_game3d ${ZANNA_TESTS_DIR}/unit/test_rt_game3d.cpp)
    target_link_libraries(test_rt_game3d PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    target_compile_definitions(test_rt_game3d PRIVATE ZANNA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    zanna_add_ctest(test_rt_game3d test_rt_game3d)
    zanna_add_test_labels(test_rt_game3d graphics3d unit)

    zanna_add_test(test_rt_game3d_diagnostics ${ZANNA_TESTS_DIR}/unit/test_rt_game3d_diagnostics.cpp)
    target_link_libraries(test_rt_game3d_diagnostics PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads)
    zanna_add_ctest(test_rt_game3d_diagnostics test_rt_game3d_diagnostics)
    zanna_add_test_labels(test_rt_game3d_diagnostics graphics3d unit)

    zanna_add_test(test_rt_game3d_thirdperson ${ZANNA_TESTS_DIR}/unit/test_rt_game3d_thirdperson.cpp)
    target_link_libraries(test_rt_game3d_thirdperson PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_game3d_thirdperson test_rt_game3d_thirdperson)
    zanna_add_test_labels(test_rt_game3d_thirdperson graphics3d unit)

    zanna_add_test(test_rt_game3d_combat ${ZANNA_TESTS_DIR}/unit/test_rt_game3d_combat.cpp)
    target_link_libraries(test_rt_game3d_combat PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_game3d_combat test_rt_game3d_combat)
    zanna_add_test_labels(test_rt_game3d_combat graphics3d unit)

    zanna_add_test(test_rt_game3d_ragdoll_time ${ZANNA_TESTS_DIR}/unit/test_rt_game3d_ragdoll_time.cpp)
    target_link_libraries(test_rt_game3d_ragdoll_time PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_game3d_ragdoll_time test_rt_game3d_ragdoll_time)
    zanna_add_test_labels(test_rt_game3d_ragdoll_time graphics3d unit)

    zanna_add_test(test_rt_game3d_cinematics ${ZANNA_TESTS_DIR}/unit/test_rt_game3d_cinematics.cpp)
    target_link_libraries(test_rt_game3d_cinematics PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_game3d_cinematics test_rt_game3d_cinematics)
    zanna_add_test_labels(test_rt_game3d_cinematics graphics3d unit)

    zanna_add_test(test_rt_game3d_dialogue_facial ${ZANNA_TESTS_DIR}/unit/test_rt_game3d_dialogue_facial.cpp)
    target_link_libraries(test_rt_game3d_dialogue_facial PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_game3d_dialogue_facial test_rt_game3d_dialogue_facial)
    zanna_add_test_labels(test_rt_game3d_dialogue_facial graphics3d unit)

    zanna_add_test(test_rt_game3d_runtime_audit ${ZANNA_TESTS_DIR}/unit/test_rt_game3d_runtime_audit.cpp)
    target_link_libraries(test_rt_game3d_runtime_audit PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_game3d_runtime_audit test_rt_game3d_runtime_audit)
    zanna_add_test_labels(test_rt_game3d_runtime_audit graphics3d unit)

    zanna_add_test(test_rt_game3d_streaming_async ${ZANNA_TESTS_DIR}/unit/test_rt_game3d_streaming_async.cpp)
    target_link_libraries(test_rt_game3d_streaming_async PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_game3d_streaming_async test_rt_game3d_streaming_async)
    zanna_add_test_labels(test_rt_game3d_streaming_async graphics3d unit)

    zanna_add_test(test_rt_game3d_hlod ${ZANNA_TESTS_DIR}/unit/test_rt_game3d_hlod.cpp)
    target_link_libraries(test_rt_game3d_hlod PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_game3d_hlod test_rt_game3d_hlod)
    zanna_add_test_labels(test_rt_game3d_hlod graphics3d unit)

    zanna_add_test(test_rt_terrain3d_upgrades ${ZANNA_TESTS_DIR}/unit/test_rt_terrain3d_upgrades.cpp)
    target_link_libraries(test_rt_terrain3d_upgrades PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_terrain3d_upgrades test_rt_terrain3d_upgrades)
    zanna_add_test_labels(test_rt_terrain3d_upgrades graphics3d unit)

    zanna_add_test(test_rt_lightbaker3d ${ZANNA_TESTS_DIR}/unit/test_rt_lightbaker3d.cpp)
    target_link_libraries(test_rt_lightbaker3d PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_lightbaker3d test_rt_lightbaker3d)
    zanna_add_test_labels(test_rt_lightbaker3d graphics3d unit)

    zanna_add_test(g3d_3dnext2_surface_probe ${ZANNA_TESTS_DIR}/unit/test_3dnext2_surface_probe.cpp)
    target_link_libraries(g3d_3dnext2_surface_probe PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads)
    zanna_add_ctest(g3d_3dnext2_surface_probe g3d_3dnext2_surface_probe)
    zanna_add_test_labels(g3d_3dnext2_surface_probe graphics3d unit contract)

    zanna_add_test(test_rt_g3d_commit_queue ${ZANNA_TESTS_DIR}/unit/test_rt_g3d_commit_queue.cpp)
    target_link_libraries(test_rt_g3d_commit_queue PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads)
    zanna_add_ctest(test_rt_g3d_commit_queue test_rt_g3d_commit_queue)
    zanna_add_test_labels(test_rt_g3d_commit_queue graphics3d unit runtime)

    zanna_add_test(test_rt_gltf ${ZANNA_TESTS_DIR}/unit/test_rt_gltf.cpp)
    target_link_libraries(test_rt_gltf PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} zanna_packaging)
    zanna_add_ctest(test_rt_gltf test_rt_gltf)
    zanna_add_test_labels(test_rt_gltf graphics3d unit)

    zanna_add_test(test_rt_asset_load_errors ${ZANNA_TESTS_DIR}/unit/test_rt_asset_load_errors.cpp)
    target_link_libraries(test_rt_asset_load_errors PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_asset_load_errors test_rt_asset_load_errors)
    zanna_add_test_labels(test_rt_asset_load_errors graphics3d unit)

    zanna_add_test(test_rt_model3d ${ZANNA_TESTS_DIR}/unit/test_rt_model3d.cpp)
    target_link_libraries(test_rt_model3d PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} zanna_packaging)
    target_compile_definitions(test_rt_model3d PRIVATE ZANNA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    zanna_add_ctest(test_rt_model3d test_rt_model3d)
    zanna_add_test_labels(test_rt_model3d graphics3d unit)

    zanna_add_test(test_rt_animcontroller3d ${ZANNA_TESTS_DIR}/unit/test_rt_animcontroller3d.cpp)
    target_link_libraries(test_rt_animcontroller3d PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_animcontroller3d test_rt_animcontroller3d)
    zanna_add_test_labels(test_rt_animcontroller3d graphics3d unit)

    zanna_add_test(test_rt_skeleton3d ${ZANNA_TESTS_DIR}/unit/test_rt_skeleton3d.cpp)
    target_link_libraries(test_rt_skeleton3d PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_skeleton3d test_rt_skeleton3d)
    zanna_add_test_labels(test_rt_skeleton3d graphics3d unit)

    zanna_add_test(test_rt_morphtarget3d ${ZANNA_TESTS_DIR}/unit/test_rt_morphtarget3d.cpp)
    target_link_libraries(test_rt_morphtarget3d PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_morphtarget3d test_rt_morphtarget3d)
    zanna_add_test_labels(test_rt_morphtarget3d graphics3d unit)

    zanna_add_test(test_rt_canvas3d_gpu_paths ${ZANNA_TESTS_DIR}/unit/test_rt_canvas3d_gpu_paths.cpp)
    target_link_libraries(test_rt_canvas3d_gpu_paths PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_canvas3d_gpu_paths test_rt_canvas3d_gpu_paths)
    zanna_add_test_labels(test_rt_canvas3d_gpu_paths graphics3d unit)

    zanna_add_test(test_vgfx3d_backend_utils ${ZANNA_TESTS_DIR}/unit/test_vgfx3d_backend_utils.c)
    target_link_libraries(test_vgfx3d_backend_utils PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_vgfx3d_backend_utils test_vgfx3d_backend_utils)
    zanna_add_test_labels(test_vgfx3d_backend_utils graphics3d unit)

    zanna_add_test(test_vgfx3d_backend_d3d11_shared ${ZANNA_TESTS_DIR}/unit/test_vgfx3d_backend_d3d11_shared.c)
    target_link_libraries(test_vgfx3d_backend_d3d11_shared PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    target_compile_definitions(test_vgfx3d_backend_d3d11_shared PRIVATE ZANNA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    zanna_add_ctest(test_vgfx3d_backend_d3d11_shared test_vgfx3d_backend_d3d11_shared)
    zanna_add_test_labels(test_vgfx3d_backend_d3d11_shared graphics3d unit)

    zanna_add_test(test_vgfx3d_backend_metal_shared ${ZANNA_TESTS_DIR}/unit/test_vgfx3d_backend_metal_shared.c)
    target_link_libraries(test_vgfx3d_backend_metal_shared PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    target_compile_definitions(test_vgfx3d_backend_metal_shared PRIVATE ZANNA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    zanna_add_ctest(test_vgfx3d_backend_metal_shared test_vgfx3d_backend_metal_shared)
    zanna_add_test_labels(test_vgfx3d_backend_metal_shared graphics3d unit)

    zanna_add_test(test_vgfx3d_backend_opengl_shared ${ZANNA_TESTS_DIR}/unit/test_vgfx3d_backend_opengl_shared.c)
    target_link_libraries(test_vgfx3d_backend_opengl_shared PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    target_compile_definitions(test_vgfx3d_backend_opengl_shared PRIVATE ZANNA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    zanna_add_ctest(test_vgfx3d_backend_opengl_shared test_vgfx3d_backend_opengl_shared)
    zanna_add_test_labels(test_vgfx3d_backend_opengl_shared graphics3d unit)

    zanna_add_test(test_vgfx3d_particle_backend_contract ${ZANNA_TESTS_DIR}/unit/test_vgfx3d_particle_backend_contract.c)
    target_link_libraries(test_vgfx3d_particle_backend_contract PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    target_compile_definitions(test_vgfx3d_particle_backend_contract PRIVATE ZANNA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    zanna_add_ctest(test_vgfx3d_particle_backend_contract test_vgfx3d_particle_backend_contract)
    zanna_add_test_labels(test_vgfx3d_particle_backend_contract graphics3d unit)

    zanna_add_test(test_rt_postfx3d_snapshot ${ZANNA_TESTS_DIR}/unit/test_rt_postfx3d_snapshot.c)
    target_link_libraries(test_rt_postfx3d_snapshot PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_postfx3d_snapshot test_rt_postfx3d_snapshot)
    zanna_add_test_labels(test_rt_postfx3d_snapshot graphics3d unit)

    zanna_add_test(test_rt_raycast3d ${ZANNA_TESTS_DIR}/unit/test_rt_raycast3d.cpp)
    target_link_libraries(test_rt_raycast3d PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_raycast3d test_rt_raycast3d)

    zanna_add_test(test_rt_physics3d ${ZANNA_TESTS_DIR}/unit/test_rt_physics3d.cpp)
    target_link_libraries(test_rt_physics3d PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_physics3d test_rt_physics3d)
    zanna_add_test_labels(test_rt_physics3d graphics3d unit)

    zanna_add_test(test_rt_gltf_draco_probe ${ZANNA_TESTS_DIR}/unit/test_rt_gltf_draco_probe.cpp)
    target_link_libraries(test_rt_gltf_draco_probe PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_gltf_draco_probe test_rt_gltf_draco_probe)
    zanna_add_test_labels(test_rt_gltf_draco_probe graphics3d unit)

    zanna_add_test(test_rt_gltf_draco_internal
            ${ZANNA_TESTS_DIR}/unit/test_rt_gltf_draco_internal.c)
    target_link_libraries(test_rt_gltf_draco_internal PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_gltf_draco_internal test_rt_gltf_draco_internal)
    zanna_add_test_labels(test_rt_gltf_draco_internal graphics3d unit)

    zanna_add_test(test_rt_transform_path ${ZANNA_TESTS_DIR}/unit/test_rt_transform_path.cpp)
    target_link_libraries(test_rt_transform_path PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_transform_path test_rt_transform_path)

    zanna_add_test(test_rt_instterrain ${ZANNA_TESTS_DIR}/unit/test_rt_instterrain.cpp)
    target_link_libraries(test_rt_instterrain PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_instterrain test_rt_instterrain)

    zanna_add_test(test_rt_navmesh_blend ${ZANNA_TESTS_DIR}/unit/test_rt_navmesh_blend.cpp)
    target_link_libraries(test_rt_navmesh_blend PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_navmesh_blend test_rt_navmesh_blend)

    zanna_add_test(test_rt_navagent3d ${ZANNA_TESTS_DIR}/unit/test_rt_navagent3d.cpp)
    target_link_libraries(test_rt_navagent3d PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_navagent3d test_rt_navagent3d)

    zanna_add_test(test_rt_sound3d_objects ${ZANNA_TESTS_DIR}/unit/test_rt_sound3d_objects.cpp)
    target_link_libraries(test_rt_sound3d_objects PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_sound3d_objects test_rt_sound3d_objects)

    zanna_add_test(test_rt_math_core ${ZANNA_TESTS_DIR}/runtime/RTMathCoreTests.cpp)
    target_link_libraries(test_rt_math_core PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_math_core test_rt_math_core)

    zanna_add_test(test_rt_numeric ${ZANNA_TESTS_DIR}/runtime/RTNumericTests.cpp)
    target_link_libraries(test_rt_numeric PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_numeric test_rt_numeric)

    zanna_add_test(test_rt_float_determinism ${ZANNA_TESTS_DIR}/runtime/FloatDeterminismTests.cpp)
    target_link_libraries(test_rt_float_determinism PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_float_determinism test_rt_float_determinism)

    zanna_add_test(
            test_rt_float_formatting
            ${ZANNA_TESTS_DIR}/runtime/FloatFormattingTests.cpp
            TEST_NAME runtime_FloatFormattingTests)
    target_link_libraries(test_rt_float_formatting PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})

    zanna_add_test(
            test_rt_int64_to_string
            ${ZANNA_TESTS_DIR}/runtime/Int64ToStringTests.cpp
            TEST_NAME runtime_Int64ToStringTests)
    target_link_libraries(test_rt_int64_to_string PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})

    zanna_add_test(test_rt_string_builder ${ZANNA_TESTS_DIR}/runtime/RTStringBuilderTests.c)
    target_link_libraries(test_rt_string_builder PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_string_builder test_rt_string_builder)

    zanna_add_test(test_rt_error_plumbing ${ZANNA_TESTS_DIR}/runtime/RtErrorPlumbingTests.cpp)
    target_link_libraries(test_rt_error_plumbing PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_error_plumbing test_rt_error_plumbing)

    zanna_add_test(test_rt_trap_contracts ${ZANNA_TESTS_DIR}/runtime/RTTrapContractTests.cpp)
    target_link_libraries(test_rt_trap_contracts PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_trap_contracts test_rt_trap_contracts)

    zanna_add_test(test_rt_errors_io ${ZANNA_TESTS_DIR}/runtime/ErrorsIoTests.c)
    target_link_libraries(test_rt_errors_io PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_errors_io test_rt_errors_io)

    zanna_add_test(test_rt_file_mode_string ${ZANNA_TESTS_DIR}/runtime/RTFileModeStringTests.c)
    target_link_libraries(test_rt_file_mode_string PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_file_mode_string test_rt_file_mode_string)

    zanna_add_test(test_rt_file_wrappers ${ZANNA_TESTS_DIR}/runtime/FileWrappersTests.c)
    target_link_libraries(test_rt_file_wrappers PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_file_wrappers test_rt_file_wrappers)

    zanna_add_test(test_rt_file_channel_io ${ZANNA_TESTS_DIR}/runtime/FileChannelIoTests.c)
    target_link_libraries(test_rt_file_channel_io PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_file_channel_io test_rt_file_channel_io)

    zanna_add_test(test_rt_file_mode ${ZANNA_TESTS_DIR}/runtime/RTFileModeTests.c)
    target_link_libraries(test_rt_file_mode PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_file_mode test_rt_file_mode)

    zanna_add_test(test_rt_path ${ZANNA_TESTS_DIR}/runtime/RTPathTests.cpp)
    target_link_libraries(test_rt_path PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_path test_rt_path)

    zanna_add_test(test_rt_dir ${ZANNA_TESTS_DIR}/runtime/RTDirTests.cpp)
    target_link_libraries(test_rt_dir PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_dir test_rt_dir)

    zanna_add_test(test_rt_watcher ${ZANNA_TESTS_DIR}/runtime/RTWatcherTests.cpp)
    target_link_libraries(test_rt_watcher PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_watcher test_rt_watcher)

    zanna_add_test(test_rt_file_ext ${ZANNA_TESTS_DIR}/runtime/RTFileExtTests.cpp)
    target_link_libraries(test_rt_file_ext PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_file_ext test_rt_file_ext)

    zanna_add_test(test_rt_asset ${ZANNA_TESTS_DIR}/runtime/RTAssetTests.cpp)
    target_link_libraries(test_rt_asset PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_asset test_rt_asset)

    zanna_add_test(test_rt_savedata ${ZANNA_TESTS_DIR}/runtime/RTSaveDataTests.cpp)
    target_link_libraries(test_rt_savedata PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_savedata test_rt_savedata)

    zanna_add_test(test_rt_math_ext ${ZANNA_TESTS_DIR}/runtime/RTMathExtTests.cpp)
    target_link_libraries(test_rt_math_ext PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_math_ext test_rt_math_ext)

    zanna_add_test(test_rt_clock ${ZANNA_TESTS_DIR}/runtime/RTClockTests.cpp)
    target_link_libraries(test_rt_clock PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_clock test_rt_clock)

    zanna_add_test(test_rt_stopwatch ${ZANNA_TESTS_DIR}/runtime/RTStopwatchTests.cpp)
    target_link_libraries(test_rt_stopwatch PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_stopwatch test_rt_stopwatch)
    set_tests_properties(test_rt_stopwatch PROPERTIES RUN_SERIAL TRUE)

    zanna_add_test(test_rt_guid ${ZANNA_TESTS_DIR}/runtime/RTGuidTests.cpp)
    target_link_libraries(test_rt_guid PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_guid test_rt_guid)

    zanna_add_test(test_rt_codec ${ZANNA_TESTS_DIR}/runtime/RTCodecTests.cpp)
    target_link_libraries(test_rt_codec PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_codec test_rt_codec)

    zanna_add_test(test_rt_ogg ${ZANNA_TESTS_DIR}/runtime/RTOggTests.cpp)
    target_link_libraries(test_rt_ogg PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_ogg test_rt_ogg)

    zanna_add_test(test_rt_vorbis_internal ${ZANNA_TESTS_DIR}/runtime/RTVorbisInternalTests.c)
    if (UNIX AND NOT APPLE)
        target_link_libraries(test_rt_vorbis_internal PRIVATE m)
    endif ()
    zanna_add_ctest(test_rt_vorbis_internal test_rt_vorbis_internal)

    zanna_add_test(test_rt_csv ${ZANNA_TESTS_DIR}/runtime/RTCsvTests.cpp)
    target_link_libraries(test_rt_csv PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_csv test_rt_csv)

    zanna_add_test(test_rt_fuzzy_match ${ZANNA_TESTS_DIR}/runtime/RTFuzzyMatchTests.cpp)
    target_link_libraries(test_rt_fuzzy_match PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_fuzzy_match test_rt_fuzzy_match)

    zanna_add_test(test_rt_json ${ZANNA_TESTS_DIR}/runtime/RTJsonTests.cpp)
    target_link_libraries(test_rt_json PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_json test_rt_json)

    zanna_add_test(test_rt_json_stream ${ZANNA_TESTS_DIR}/runtime/RTJsonStreamTests.cpp)
    target_link_libraries(test_rt_json_stream PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_json_stream test_rt_json_stream)

    zanna_add_test(test_rt_regex ${ZANNA_TESTS_DIR}/runtime/RTRegexTests.cpp)
    target_link_libraries(test_rt_regex PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_regex test_rt_regex)

    zanna_add_test(test_rt_datetime ${ZANNA_TESTS_DIR}/runtime/RTDatetimeTests.cpp)
    target_link_libraries(test_rt_datetime PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_datetime test_rt_datetime)

    zanna_add_test(test_rt_aes ${ZANNA_TESTS_DIR}/runtime/RTAesTests.cpp)
    target_link_libraries(test_rt_aes PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_aes test_rt_aes)

    zanna_add_test(test_rt_bigint ${ZANNA_TESTS_DIR}/runtime/RTBigintTests.cpp)
    target_link_libraries(test_rt_bigint PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_bigint test_rt_bigint)

    zanna_add_test(test_rt_bigint_contracts ${ZANNA_TESTS_DIR}/runtime/RTBigIntContractTests.cpp)
    target_link_libraries(test_rt_bigint_contracts PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_bigint_contracts test_rt_bigint_contracts)

    zanna_add_test(test_rt_xml ${ZANNA_TESTS_DIR}/runtime/RTXmlTests.cpp)
    target_link_libraries(test_rt_xml PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_xml test_rt_xml)

    zanna_add_test(test_rt_hash ${ZANNA_TESTS_DIR}/runtime/RTHashTests.cpp)
    target_link_libraries(test_rt_hash PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_hash test_rt_hash)

    zanna_add_test(test_rt_crypto ${ZANNA_TESTS_DIR}/runtime/RTCryptoTests.cpp)
    target_link_libraries(test_rt_crypto PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_crypto test_rt_crypto)

    zanna_add_test(test_rt_cipher ${ZANNA_TESTS_DIR}/runtime/RTCipherTests.cpp)
    target_link_libraries(test_rt_cipher PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_cipher test_rt_cipher)
    zanna_add_test_labels(test_rt_cipher slow)

    zanna_add_test(test_rt_stream ${ZANNA_TESTS_DIR}/runtime/RTStreamTests.cpp)
    target_link_libraries(test_rt_stream PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_stream test_rt_stream)

    zanna_add_test(test_rt_seq_functional ${ZANNA_TESTS_DIR}/runtime/RTSeqFunctionalTests.cpp)
    target_link_libraries(test_rt_seq_functional PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_seq_functional test_rt_seq_functional)

    zanna_add_test(test_rt_password ${ZANNA_TESTS_DIR}/runtime/RTPasswordTests.cpp)
    target_link_libraries(test_rt_password PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_password test_rt_password)

    zanna_add_test(test_rt_duration ${ZANNA_TESTS_DIR}/runtime/RTDurationTests.cpp)
    target_link_libraries(test_rt_duration PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_duration test_rt_duration)

    zanna_add_test(test_rt_locale ${ZANNA_TESTS_DIR}/runtime/RTLocaleTests.cpp)
    target_link_libraries(test_rt_locale PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_locale test_rt_locale)

    if (WIN32)
        zanna_add_test(test_rt_windows_runtime
                ${ZANNA_TESTS_DIR}/runtime/RTWindowsRuntimeTests.cpp)
        target_link_libraries(test_rt_windows_runtime PRIVATE
                ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads)
        target_compile_definitions(test_rt_windows_runtime PRIVATE
                ZANNA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
        zanna_add_ctest(test_rt_windows_runtime test_rt_windows_runtime)
    elseif (UNIX)
        zanna_add_test(test_rt_posix_socket_platform
                ${ZANNA_TESTS_DIR}/runtime/RTPosixSocketPlatformTests.cpp)
        target_link_libraries(test_rt_posix_socket_platform PRIVATE
                ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads)
        zanna_add_ctest(test_rt_posix_socket_platform test_rt_posix_socket_platform)
        zanna_add_test_labels(test_rt_posix_socket_platform runtime unit)

        if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
            zanna_add_isolated_runtime_test(test_rt_posix_entropy_platform
                    ${ZANNA_TESTS_DIR}/runtime/RTPosixEntropyPlatformTests.c)
            zanna_add_test_labels(test_rt_posix_entropy_platform
                    runtime unit requires_linux)

            zanna_add_isolated_runtime_test(test_rt_posix_socket_faults
                    ${ZANNA_TESTS_DIR}/runtime/RTPosixSocketFaultTests.c)
            zanna_add_test_labels(test_rt_posix_socket_faults
                    runtime unit requires_linux)
        endif ()
    endif ()

    zanna_add_test(test_rt_locale_manager ${ZANNA_TESTS_DIR}/runtime/RTLocaleManagerTests.cpp)
    target_link_libraries(test_rt_locale_manager PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_locale_manager test_rt_locale_manager)

    zanna_add_test(test_rt_locale_info ${ZANNA_TESTS_DIR}/runtime/RTLocaleInfoTests.cpp)
    target_link_libraries(test_rt_locale_info PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_locale_info test_rt_locale_info)

    zanna_add_test(test_rt_plural_rules ${ZANNA_TESTS_DIR}/runtime/RTPluralRulesTests.cpp)
    target_link_libraries(test_rt_plural_rules PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_plural_rules test_rt_plural_rules)

    zanna_add_test(test_rt_locale_numformat ${ZANNA_TESTS_DIR}/runtime/RTLocaleNumberFormatTests.cpp)
    target_link_libraries(test_rt_locale_numformat PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_locale_numformat test_rt_locale_numformat)

    zanna_add_test(test_rt_dateformat ${ZANNA_TESTS_DIR}/runtime/RTDateFormatTests.cpp)
    target_link_libraries(test_rt_dateformat PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_dateformat test_rt_dateformat)

    zanna_add_test(test_rt_reltime_format ${ZANNA_TESTS_DIR}/runtime/RTRelativeTimeFormatTests.cpp)
    target_link_libraries(test_rt_reltime_format PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_reltime_format test_rt_reltime_format)

    zanna_add_test(test_rt_message_bundle ${ZANNA_TESTS_DIR}/runtime/RTMessageBundleTests.cpp)
    target_link_libraries(test_rt_message_bundle PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_message_bundle test_rt_message_bundle)

    zanna_add_test(test_rt_list_format ${ZANNA_TESTS_DIR}/runtime/RTListFormatTests.cpp)
    target_link_libraries(test_rt_list_format PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_list_format test_rt_list_format)

    zanna_add_test(test_rt_text_direction ${ZANNA_TESTS_DIR}/runtime/RTTextDirectionTests.cpp)
    target_link_libraries(test_rt_text_direction PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_text_direction test_rt_text_direction)

    zanna_add_test(test_rt_collator ${ZANNA_TESTS_DIR}/runtime/RTCollatorTests.cpp)
    target_link_libraries(test_rt_collator PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_collator test_rt_collator)

    zanna_add_test(test_rt_glob ${ZANNA_TESTS_DIR}/runtime/RTGlobTests.cpp)
    target_link_libraries(test_rt_glob PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_glob test_rt_glob)

    zanna_add_test(test_rt_ide_workspace ${ZANNA_TESTS_DIR}/runtime/RTIdeWorkspaceTests.cpp)
    target_link_libraries(test_rt_ide_workspace PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_ide_workspace test_rt_ide_workspace)

    zanna_add_test(test_rt_tempfile ${ZANNA_TESTS_DIR}/runtime/RTTempFileTests.cpp)
    target_link_libraries(test_rt_tempfile PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_tempfile test_rt_tempfile)

    zanna_add_test(test_rt_result ${ZANNA_TESTS_DIR}/runtime/RTResultTests.cpp)
    target_link_libraries(test_rt_result PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_result test_rt_result)

    zanna_add_test(test_rt_option ${ZANNA_TESTS_DIR}/runtime/RTOptionTests.cpp)
    target_link_libraries(test_rt_option PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_option test_rt_option)

    zanna_add_test(test_rt_core_ownership ${ZANNA_TESTS_DIR}/runtime/RTCoreOwnershipTests.cpp)
    target_link_libraries(test_rt_core_ownership PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_core_ownership test_rt_core_ownership)

    zanna_add_test(test_rt_deque ${ZANNA_TESTS_DIR}/runtime/RTDequeTests.cpp)
    target_link_libraries(test_rt_deque PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_deque test_rt_deque)

    zanna_add_test(test_rt_convert_coll ${ZANNA_TESTS_DIR}/runtime/RTConvertCollTests.cpp)
    target_link_libraries(test_rt_convert_coll PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_convert_coll test_rt_convert_coll)

    zanna_add_test(test_rt_collections_correctness ${ZANNA_TESTS_DIR}/runtime/RTCollectionsCorrectnessTests.cpp)
    target_link_libraries(test_rt_collections_correctness PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_collections_correctness test_rt_collections_correctness)

    zanna_add_test(test_rt_numbuf ${ZANNA_TESTS_DIR}/runtime/RTNumBufTests.cpp)
    target_link_libraries(test_rt_numbuf PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_numbuf test_rt_numbuf)

    zanna_add_test(test_rt_set ${ZANNA_TESTS_DIR}/runtime/RTSetTests.cpp)
    target_link_libraries(test_rt_set PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_set test_rt_set)

    zanna_add_test(test_rt_seq_box ${ZANNA_TESTS_DIR}/runtime/RTSeqBoxTests.cpp)
    target_link_libraries(test_rt_seq_box PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_seq_box test_rt_seq_box)

    zanna_add_test(test_rt_list_box ${ZANNA_TESTS_DIR}/runtime/RTListBoxTests.cpp)
    target_link_libraries(test_rt_list_box PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_list_box test_rt_list_box)

    zanna_add_test(test_rt_sortedset ${ZANNA_TESTS_DIR}/runtime/RTSortedSetTests.cpp)
    target_link_libraries(test_rt_sortedset PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_sortedset test_rt_sortedset)

    zanna_add_test(test_rt_scanner ${ZANNA_TESTS_DIR}/runtime/RTScannerTests.cpp)
    target_link_libraries(test_rt_scanner PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_scanner test_rt_scanner)

    zanna_add_test(test_rt_dateonly ${ZANNA_TESTS_DIR}/runtime/RTDateOnlyTests.cpp)
    target_link_libraries(test_rt_dateonly PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_dateonly test_rt_dateonly)

    zanna_add_test(test_rt_textwrap ${ZANNA_TESTS_DIR}/runtime/RTTextWrapTests.cpp)
    target_link_libraries(test_rt_textwrap PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_textwrap test_rt_textwrap)

    zanna_add_test(test_rt_lazy ${ZANNA_TESTS_DIR}/runtime/RTLazyTests.cpp)
    target_link_libraries(test_rt_lazy PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_lazy test_rt_lazy)

    zanna_add_test(test_rt_lazyseq ${ZANNA_TESTS_DIR}/runtime/RTLazySeqTests.cpp)
    target_link_libraries(test_rt_lazyseq PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_lazyseq test_rt_lazyseq)

    zanna_add_test(test_rt_pattern ${ZANNA_TESTS_DIR}/runtime/RTPatternTests.cpp)
    target_link_libraries(test_rt_pattern PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_pattern test_rt_pattern)

    zanna_add_test(test_rt_compiled_pattern ${ZANNA_TESTS_DIR}/runtime/RTCompiledPatternTests.cpp)
    target_link_libraries(test_rt_compiled_pattern PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_compiled_pattern test_rt_compiled_pattern)

    zanna_add_test(test_rt_template ${ZANNA_TESTS_DIR}/runtime/RTTemplateTests.cpp)
    target_link_libraries(test_rt_template PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_template test_rt_template)

    zanna_add_test(test_rt_compress ${ZANNA_TESTS_DIR}/runtime/RTCompressTests.cpp)
    target_link_libraries(test_rt_compress PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_compress test_rt_compress)

    zanna_add_test(test_rt_archive ${ZANNA_TESTS_DIR}/runtime/RTArchiveTests.cpp)
    target_link_libraries(test_rt_archive PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_archive test_rt_archive)

    zanna_add_test(test_rt_canvas_window ${ZANNA_TESTS_DIR}/runtime/RTCanvasWindowTests.cpp)
    target_link_libraries(test_rt_canvas_window PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_canvas_window test_rt_canvas_window)
    zanna_add_test_labels(test_rt_canvas_window requires_display)

    zanna_add_test(test_rt_network ${ZANNA_TESTS_DIR}/runtime/RTNetworkTests.cpp)
    target_link_libraries(test_rt_network PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_network test_rt_network)
    zanna_add_test_labels(test_rt_network requires_local_bind)

    zanna_add_test(test_rt_network_timeout ${ZANNA_TESTS_DIR}/runtime/RTNetworkTimeoutTests.cpp)
    target_link_libraries(test_rt_network_timeout PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_network_timeout test_rt_network_timeout)
    zanna_add_test_labels(test_rt_network_timeout requires_local_bind)

    zanna_add_test(test_rt_network_harden ${ZANNA_TESTS_DIR}/runtime/RTNetworkHardenTests.cpp)
    target_link_libraries(test_rt_network_harden PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_network_harden test_rt_network_harden)
    zanna_add_test_labels(test_rt_network_harden requires_local_bind)

    zanna_add_test(test_rt_trap_return_network ${ZANNA_TESTS_DIR}/runtime/RTTrapReturnNetworkTests.cpp)
    target_link_libraries(test_rt_trap_return_network PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_trap_return_network test_rt_trap_return_network)

    zanna_add_test(test_rt_async_socket_init ${ZANNA_TESTS_DIR}/runtime/RTAsyncSocketInitTests.cpp)
    target_link_libraries(test_rt_async_socket_init PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads)
    zanna_add_ctest(test_rt_async_socket_init test_rt_async_socket_init)

    zanna_add_test(test_rt_multipart ${ZANNA_TESTS_DIR}/runtime/RTMultipartTests.cpp)
    target_link_libraries(test_rt_multipart PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_multipart test_rt_multipart)

    zanna_add_test(test_rt_websocket ${ZANNA_TESTS_DIR}/runtime/RTWebSocketTests.cpp)
    target_link_libraries(test_rt_websocket PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_websocket test_rt_websocket)
    zanna_add_test_labels(test_rt_websocket requires_local_bind)

    zanna_add_test(test_rt_tls_cert ${ZANNA_TESTS_DIR}/runtime/RTTlsCertTests.cpp)
    target_link_libraries(test_rt_tls_cert PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_tls_cert test_rt_tls_cert)

    zanna_add_test(test_rt_tls_wrapper ${ZANNA_TESTS_DIR}/runtime/RTTlsWrapperTests.cpp)
    target_link_libraries(test_rt_tls_wrapper PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_tls_wrapper test_rt_tls_wrapper)

    zanna_add_test(test_rt_http2 ${ZANNA_TESTS_DIR}/runtime/RTHttp2Tests.cpp)
    target_link_libraries(test_rt_http2 PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_http2 test_rt_http2)

    zanna_add_test(test_rt_restclient ${ZANNA_TESTS_DIR}/runtime/RTRestClientTests.cpp)
    target_link_libraries(test_rt_restclient PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_restclient test_rt_restclient)
    zanna_add_test_labels(test_rt_restclient requires_local_bind)
    set_tests_properties(test_rt_restclient PROPERTIES RUN_SERIAL TRUE TIMEOUT 90)

    zanna_add_test(test_rt_http_client ${ZANNA_TESTS_DIR}/runtime/RTHttpClientTests.cpp)
    target_link_libraries(test_rt_http_client PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads)
    zanna_add_ctest(test_rt_http_client test_rt_http_client)
    zanna_add_test_labels(test_rt_http_client requires_local_bind)
    set_tests_properties(test_rt_http_client PROPERTIES RUN_SERIAL TRUE TIMEOUT 90)

    zanna_add_test(test_rt_http_server ${ZANNA_TESTS_DIR}/runtime/RTHttpServerTests.cpp)
    target_link_libraries(test_rt_http_server PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads)
    zanna_add_ctest(test_rt_http_server test_rt_http_server)
    zanna_add_test_labels(test_rt_http_server requires_local_bind)
    set_tests_properties(test_rt_http_server PROPERTIES RUN_SERIAL TRUE TIMEOUT 90)

    zanna_add_test(test_rt_https_server ${ZANNA_TESTS_DIR}/runtime/RTHttpsServerTests.cpp)
    target_link_libraries(test_rt_https_server PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads)
    zanna_add_ctest(test_rt_https_server test_rt_https_server)
    zanna_add_test_labels(test_rt_https_server requires_local_bind slow)
    set_tests_properties(test_rt_https_server PROPERTIES RUN_SERIAL TRUE TIMEOUT 90)

    zanna_add_test(test_rt_network_highlevel ${ZANNA_TESTS_DIR}/runtime/RTHighLevelNetworkTests.cpp)
    target_link_libraries(test_rt_network_highlevel PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_network_highlevel test_rt_network_highlevel)
    zanna_add_test_labels(test_rt_network_highlevel requires_local_bind slow)
    set_tests_properties(test_rt_network_highlevel PROPERTIES RUN_SERIAL TRUE TIMEOUT 240)
    foreach (_local_bind_test
            test_rt_network
            test_rt_network_timeout
            test_rt_network_harden
            test_rt_websocket
            test_rt_restclient
            test_rt_http_client
            test_rt_http_server
            test_rt_https_server
            test_rt_network_highlevel)
        set_tests_properties(${_local_bind_test} PROPERTIES RESOURCE_LOCK local_bind)
    endforeach ()

    zanna_add_test(test_rt_bag ${ZANNA_TESTS_DIR}/runtime/RTBagTests.cpp)
    target_link_libraries(test_rt_bag PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_bag test_rt_bag)

    zanna_add_test(test_rt_binfile ${ZANNA_TESTS_DIR}/runtime/RTBinFileTests.cpp)
    target_link_libraries(test_rt_binfile PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_binfile test_rt_binfile)

    zanna_add_test(test_rt_memstream ${ZANNA_TESTS_DIR}/runtime/RTMemStreamTests.cpp)
    target_link_libraries(test_rt_memstream PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_memstream test_rt_memstream)

    zanna_add_test(test_rt_linereader ${ZANNA_TESTS_DIR}/runtime/RTLineReaderTests.cpp)
    target_link_libraries(test_rt_linereader PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_linereader test_rt_linereader)

    zanna_add_test(test_rt_linewriter ${ZANNA_TESTS_DIR}/runtime/RTLineWriterTests.cpp)
    target_link_libraries(test_rt_linewriter PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_linewriter test_rt_linewriter)

    zanna_add_test(test_rt_disk_full ${ZANNA_TESTS_DIR}/runtime/RTDiskFullTests.cpp)
    target_link_libraries(test_rt_disk_full PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_disk_full test_rt_disk_full)

    zanna_add_test(test_rt_canvas_unavailable ${ZANNA_TESTS_DIR}/runtime/RTCanvasUnavailableTests.cpp)
    target_link_libraries(test_rt_canvas_unavailable PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_canvas_unavailable test_rt_canvas_unavailable)
    zanna_add_test_labels(test_rt_canvas_unavailable requires_graphics_disabled)

    zanna_add_test(test_rt_graphics_surface_link ${ZANNA_TESTS_DIR}/runtime/RTGraphicsSurfaceLinkTests.cpp)
    target_link_libraries(test_rt_graphics_surface_link PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_graphics_surface_link test_rt_graphics_surface_link)
    zanna_add_test_labels(test_rt_graphics_surface_link requires_graphics_disabled)

    zanna_add_test(test_rt_audio_unavailable ${ZANNA_TESTS_DIR}/runtime/RTAudioUnavailableTests.cpp)
    target_link_libraries(test_rt_audio_unavailable PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    if (NOT ZANNA_AUDIO_MODE STREQUAL "OFF" AND TARGET zannaaud)
        target_compile_definitions(test_rt_audio_unavailable PRIVATE ZANNA_ENABLE_AUDIO=1)
    endif ()
    zanna_add_ctest(test_rt_audio_unavailable test_rt_audio_unavailable)
    zanna_add_test_labels(test_rt_audio_unavailable requires_audio_disabled)

    zanna_add_test(test_rt_audio_surface_link ${ZANNA_TESTS_DIR}/runtime/RTAudioSurfaceLinkTests.cpp)
    target_link_libraries(test_rt_audio_surface_link PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_audio_surface_link test_rt_audio_surface_link)
    zanna_add_test_labels(test_rt_audio_surface_link requires_audio_disabled)

    zanna_add_isolated_runtime_test(test_rt_achievement_contract
            ${ZANNA_TESTS_DIR}/runtime/RTAchievementTests.cpp
            ${CMAKE_SOURCE_DIR}/src/runtime/game/rt_achievement.c)

    zanna_add_isolated_runtime_test(test_rt_sound3d_contract
            ${ZANNA_TESTS_DIR}/runtime/RTSound3DTests.cpp
            ${CMAKE_SOURCE_DIR}/src/runtime/audio/rt_audio_diagnostics.c
            ${CMAKE_SOURCE_DIR}/src/runtime/audio/rt_sound3d.c)
    target_compile_definitions(test_rt_sound3d_contract PRIVATE
            RT_NO_MAIN_THREAD_ASSERT=1)

    zanna_add_isolated_runtime_test(test_rt_videowidget_contract
            ${ZANNA_TESTS_DIR}/runtime/RTVideoWidgetContractTests.cpp
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/gui/rt_videowidget.c)
    target_compile_definitions(test_rt_videowidget_contract PRIVATE
            ZANNA_ENABLE_GRAPHICS=1
            RT_NO_MAIN_THREAD_ASSERT=1)

    zanna_add_isolated_runtime_test(test_rt_particles3d_contract
            ${ZANNA_TESTS_DIR}/runtime/RTParticles3DContractTests.cpp
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/3d/world/rt_particles3d.c)
    target_compile_definitions(test_rt_particles3d_contract PRIVATE
            ZANNA_ENABLE_GRAPHICS=1
            RT_G3D_ALLOW_STACK_FIXTURES=1)

    zanna_add_isolated_runtime_test(test_rt_water3d_contract
            ${ZANNA_TESTS_DIR}/runtime/RTWater3DContractTests.cpp
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/3d/world/rt_water3d.c)
    target_compile_definitions(test_rt_water3d_contract PRIVATE
            ZANNA_ENABLE_GRAPHICS=1
            RT_G3D_ALLOW_STACK_FIXTURES=1
            RT_PIXELS_INTERNAL_ASSUME_STRUCT_HANDLE=1)

    zanna_add_isolated_runtime_test(test_rt_vegetation3d_contract
            ${ZANNA_TESTS_DIR}/runtime/RTVegetation3DContractTests.cpp
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/3d/world/rt_vegetation3d.c)
    target_compile_definitions(test_rt_vegetation3d_contract PRIVATE
            ZANNA_ENABLE_GRAPHICS=1
            RT_G3D_ALLOW_STACK_FIXTURES=1
            RT_PIXELS_INTERNAL_ASSUME_STRUCT_HANDLE=1)

    zanna_add_test(test_rt_graphics3d_robustness
            ${ZANNA_TESTS_DIR}/runtime/RTGraphics3DRobustnessTests.cpp)
    target_link_libraries(test_rt_graphics3d_robustness PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_graphics3d_robustness test_rt_graphics3d_robustness)
    zanna_add_test_labels(test_rt_graphics3d_robustness graphics3d runtime)

    zanna_add_test(test_rt_file_exhaustion ${ZANNA_TESTS_DIR}/runtime/RTFileExhaustionTests.cpp)
    target_link_libraries(test_rt_file_exhaustion PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_file_exhaustion test_rt_file_exhaustion)

    zanna_add_test(test_rt_exec ${ZANNA_TESTS_DIR}/runtime/RTExecTests.cpp)
    target_link_libraries(test_rt_exec PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_exec test_rt_exec)

    zanna_add_test(test_rt_machine ${ZANNA_TESTS_DIR}/runtime/RTMachineTests.cpp)
    target_link_libraries(test_rt_machine PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_machine test_rt_machine)

    zanna_add_test(test_rt_bits ${ZANNA_TESTS_DIR}/runtime/RTBitsTests.cpp)
    target_link_libraries(test_rt_bits PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_bits test_rt_bits)

    zanna_add_test(test_rt_diag ${ZANNA_TESTS_DIR}/runtime/RTDiagTests.cpp)
    target_link_libraries(test_rt_diag PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_diag test_rt_diag)

    zanna_add_test(test_rt_fmt ${ZANNA_TESTS_DIR}/runtime/RTFmtTests.cpp)
    target_link_libraries(test_rt_fmt PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_fmt test_rt_fmt)

    zanna_add_test(test_rt_log ${ZANNA_TESTS_DIR}/runtime/RTLogTests.cpp)
    target_link_libraries(test_rt_log PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_log test_rt_log)

    zanna_add_test(test_rt_countdown ${ZANNA_TESTS_DIR}/runtime/RTCountdownTests.cpp)
    target_link_libraries(test_rt_countdown PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_countdown test_rt_countdown)

    zanna_add_test(test_rt_parse ${ZANNA_TESTS_DIR}/runtime/RTParseTests.cpp)
    target_link_libraries(test_rt_parse PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_parse test_rt_parse)

    zanna_add_test(test_rt_bool_std ${ZANNA_TESTS_DIR}/runtime/RTBoolStdTests.cpp)
    target_link_libraries(test_rt_bool_std PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_bool_std test_rt_bool_std)

    zanna_add_test(test_rt_vec2 ${ZANNA_TESTS_DIR}/runtime/RTVec2Tests.cpp)
    target_link_libraries(test_rt_vec2 PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_vec2 test_rt_vec2)

    zanna_add_test(test_rt_vec3 ${ZANNA_TESTS_DIR}/runtime/RTVec3Tests.cpp)
    target_link_libraries(test_rt_vec3 PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_vec3 test_rt_vec3)

    zanna_add_test(test_rt_quat ${ZANNA_TESTS_DIR}/runtime/RTQuatTests.cpp)
    target_link_libraries(test_rt_quat PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_quat test_rt_quat)

    zanna_add_test(test_rt_mat_eq ${ZANNA_TESTS_DIR}/runtime/RTMatEqTests.cpp)
    target_link_libraries(test_rt_mat_eq PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_mat_eq test_rt_mat_eq)

    zanna_add_test(test_rt_spline ${ZANNA_TESTS_DIR}/runtime/RTSplineTests.cpp)
    target_link_libraries(test_rt_spline PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_spline test_rt_spline)

    zanna_add_test(test_rt_math_handle_validation
            ${ZANNA_TESTS_DIR}/runtime/RTMathHandleValidationTests.cpp)
    target_link_libraries(test_rt_math_handle_validation PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_math_handle_validation test_rt_math_handle_validation)

    zanna_add_test(test_rt_pixels ${ZANNA_TESTS_DIR}/runtime/RTPixelsTests.cpp)
    target_link_libraries(test_rt_pixels PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_pixels test_rt_pixels)

    zanna_add_test(test_rt_pixels_draw ${ZANNA_TESTS_DIR}/runtime/RTPixelsDrawTests.cpp)
    target_link_libraries(test_rt_pixels_draw PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_pixels_draw test_rt_pixels_draw)

    zanna_add_test(test_rt_graphics2d ${ZANNA_TESTS_DIR}/runtime/RTGraphics2DTests.cpp)
    target_link_libraries(test_rt_graphics2d PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_graphics2d test_rt_graphics2d)

    zanna_add_test(test_rt_scene ${ZANNA_TESTS_DIR}/runtime/RTSceneTests.cpp)
    target_link_libraries(test_rt_scene PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_scene test_rt_scene)

    zanna_add_test(test_rt_scene_editor ${ZANNA_TESTS_DIR}/runtime/RTSceneEditorTests.cpp)
    target_link_libraries(test_rt_scene_editor PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} zanna_packaging)
    target_compile_definitions(test_rt_scene_editor PRIVATE ZANNA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    zanna_add_ctest(test_rt_scene_editor test_rt_scene_editor)

    # Consolidated: RTSpriteBatchTests + RTSpriteSheetTests → test_rt_sprite_consolidated
    zanna_add_test(test_rt_sprite_consolidated ${ZANNA_TESTS_DIR}/runtime/test_rt_sprite_consolidated.cpp)
    target_link_libraries(test_rt_sprite_consolidated PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_sprite_consolidated test_rt_sprite_consolidated)

    zanna_add_isolated_runtime_test(test_rt_spritebatch_contract
            ${ZANNA_TESTS_DIR}/runtime/RTSpriteBatchContractTests.cpp
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/2d/rt_spritebatch.c)
    target_compile_definitions(test_rt_spritebatch_contract PRIVATE
            RT_PIXELS_INTERNAL_ASSUME_STRUCT_HANDLE=1)

    zanna_add_isolated_runtime_test(test_rt_sprite_contract
            ${ZANNA_TESTS_DIR}/runtime/RTSpriteContractTests.cpp
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/2d/rt_sprite.c)
    target_compile_definitions(test_rt_sprite_contract PRIVATE
            RT_PIXELS_INTERNAL_ASSUME_STRUCT_HANDLE=1)
    if (WIN32)
        target_sources(test_rt_sprite_contract PRIVATE
                ${ZANNA_TESTS_DIR}/runtime/RTFilePathUtf8WideShim.c)
    endif ()

    zanna_add_test(test_rt_listview ${ZANNA_TESTS_DIR}/runtime/RTListViewTests.cpp)
    target_link_libraries(test_rt_listview PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_listview test_rt_listview)

    zanna_add_test(test_rt_gui_runtime ${ZANNA_TESTS_DIR}/runtime/RTGuiRuntimeTests.c)
    target_link_libraries(test_rt_gui_runtime PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    target_compile_definitions(test_rt_gui_runtime PRIVATE ZANNA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    if (TARGET zannagfx)
        target_compile_definitions(test_rt_gui_runtime PRIVATE ZANNA_ENABLE_GRAPHICS)
        target_link_libraries(test_rt_gui_runtime PRIVATE zannagfx zannagui)
    endif ()
    zanna_add_ctest(test_rt_gui_runtime test_rt_gui_runtime)
    zanna_add_test_labels(test_rt_gui_runtime requires_display)
    set_tests_properties(test_rt_gui_runtime PROPERTIES RESOURCE_LOCK zanna_display)

    if (APPLE AND TARGET zannagfx)
        zanna_add_test(test_rt_gui_macos_menu
                ${ZANNA_TESTS_DIR}/runtime/RTGuiMacMenuTests.mm)
        target_compile_definitions(test_rt_gui_macos_menu PRIVATE
                ZANNA_ENABLE_GRAPHICS
                _DARWIN_C_SOURCE)
        target_link_libraries(test_rt_gui_macos_menu PRIVATE
                ${ZANNA_RUNTIME_TEST_LIBS}
                zannagfx
                zannagui)
        # Cocoa exposes an unbundled executable's leaf name as the application
        # identity in the system menu bar. Match the shipping macOS payload so
        # this test exercises the actual product-name contract.
        set_target_properties(test_rt_gui_macos_menu PROPERTIES
                OUTPUT_NAME "Zanna Studio")
        zanna_add_ctest(test_rt_gui_macos_menu test_rt_gui_macos_menu)
        zanna_add_test_labels(test_rt_gui_macos_menu
                runtime gui requires_macos requires_display)
        set_tests_properties(test_rt_gui_macos_menu PROPERTIES
                RESOURCE_LOCK zanna_display)
    endif ()

    if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
        (ZANNAGFX_BACKEND STREQUAL "Wayland" OR ZANNAGFX_BACKEND STREQUAL "LinuxAuto" OR
         ZANNAGFX_BACKEND STREQUAL "X11"))
        zanna_add_test(test_rt_gui_linux_platform
                ${ZANNA_TESTS_DIR}/runtime/RTGuiLinuxPlatformTests.c)
        target_link_libraries(test_rt_gui_linux_platform PRIVATE
                ${ZANNA_RUNTIME_TEST_LIBS} zannagfx zannagui)
        zanna_add_ctest(test_rt_gui_linux_platform test_rt_gui_linux_platform)
        zanna_add_test_labels(test_rt_gui_linux_platform runtime gui unit requires_linux)

        zanna_add_test(test_rt_gui_atspi_linux
                ${ZANNA_TESTS_DIR}/runtime/RTGuiAtspiLinuxTests.c)
        target_link_libraries(test_rt_gui_atspi_linux PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
        target_compile_definitions(test_rt_gui_atspi_linux PRIVATE ZANNA_ENABLE_GRAPHICS)
        target_link_libraries(test_rt_gui_atspi_linux PRIVATE zannagfx zannagui)
        zanna_add_ctest(test_rt_gui_atspi_linux test_rt_gui_atspi_linux)
        zanna_add_test_labels(test_rt_gui_atspi_linux runtime gui requires_linux requires_display)
        set_tests_properties(test_rt_gui_atspi_linux PROPERTIES
                RESOURCE_LOCK zanna_display
                TIMEOUT 60)
    endif ()

    zanna_add_test(test_rt_gui_availability
            ${ZANNA_TESTS_DIR}/runtime/RTGuiAvailabilityTests.cpp)
    target_link_libraries(test_rt_gui_availability PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_gui_availability test_rt_gui_availability)
    zanna_add_test_labels(test_rt_gui_availability runtime gui contract)

    # Headless vtable tests for the Windows UI Automation provider bridge.
    # No UIA client or HWND is involved, so this runs on any Windows session.
    if (WIN32 AND TARGET zannagfx)
        zanna_add_test(test_rt_uia_provider ${ZANNA_TESTS_DIR}/runtime/RTUiaProviderTests.c)
        target_link_libraries(test_rt_uia_provider PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
        target_compile_definitions(test_rt_uia_provider PRIVATE ZANNA_ENABLE_GRAPHICS)
        target_link_libraries(test_rt_uia_provider PRIVATE zannagfx zannagui oleaut32)
        zanna_add_ctest(test_rt_uia_provider test_rt_uia_provider)
        zanna_add_test_labels(test_rt_uia_provider runtime gui accessibility)
    endif ()

    zanna_add_test(test_rt_gui_ide ${ZANNA_TESTS_DIR}/runtime/RTGuiIdeTests.cpp)
    target_link_libraries(test_rt_gui_ide PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_gui_ide test_rt_gui_ide)

    zanna_add_test(test_rt_canvas_ext ${ZANNA_TESTS_DIR}/runtime/RTCanvasExtTests.cpp)
    target_link_libraries(test_rt_canvas_ext PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_canvas_ext test_rt_canvas_ext)

    zanna_add_isolated_runtime_test(test_rt_canvas_contract
            ${ZANNA_TESTS_DIR}/runtime/RTCanvasContractTests.cpp
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/2d/rt_drawing.c
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/2d/rt_drawing_advanced.c
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/2d/rt_color.c)
    target_compile_definitions(test_rt_canvas_contract PRIVATE
            ZANNA_ENABLE_GRAPHICS=1
            RT_DRAWING_ADVANCED_NO_PATH2D=1
            RT_PIXELS_INTERNAL_ASSUME_STRUCT_HANDLE=1)

    zanna_add_isolated_runtime_test(test_rt_canvas_state_contract
            ${ZANNA_TESTS_DIR}/runtime/RTCanvasStateContractTests.cpp
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/2d/rt_canvas.c)
    target_compile_definitions(test_rt_canvas_state_contract PRIVATE ZANNA_ENABLE_GRAPHICS=1)

    zanna_add_isolated_runtime_test(test_rt_bitmapfont_contract
            ${ZANNA_TESTS_DIR}/runtime/RTBitmapFontContractTests.cpp
            ${CMAKE_SOURCE_DIR}/src/runtime/graphics/text/rt_bitmapfont.c)
    target_compile_definitions(test_rt_bitmapfont_contract PRIVATE ZANNA_ENABLE_GRAPHICS=1)
    if (WIN32)
        target_sources(test_rt_bitmapfont_contract PRIVATE
                ${ZANNA_TESTS_DIR}/runtime/RTFilePathUtf8WideShim.c)
    endif ()

    zanna_add_test(test_rt_canvas_frame ${ZANNA_TESTS_DIR}/runtime/RTCanvasFrameTests.cpp)
    target_link_libraries(test_rt_canvas_frame PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    if (TARGET zannagfx)
        target_compile_definitions(test_rt_canvas_frame PRIVATE ZANNA_ENABLE_GRAPHICS)
        target_link_libraries(test_rt_canvas_frame PRIVATE zannagfx)
    endif ()
    zanna_add_ctest(test_rt_canvas_frame test_rt_canvas_frame)

    zanna_add_test(test_rt_canvas_text_layout ${ZANNA_TESTS_DIR}/runtime/RTCanvasTextLayoutTests.cpp)
    target_link_libraries(test_rt_canvas_text_layout PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    if (TARGET zannagfx)
        target_compile_definitions(test_rt_canvas_text_layout PRIVATE ZANNA_ENABLE_GRAPHICS)
        target_link_libraries(test_rt_canvas_text_layout PRIVATE zannagfx)
    endif ()
    zanna_add_ctest(test_rt_canvas_text_layout test_rt_canvas_text_layout)

    zanna_add_test(test_rt_keyboard ${ZANNA_TESTS_DIR}/runtime/RTKeyboardTests.cpp)
    target_link_libraries(test_rt_keyboard PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_keyboard test_rt_keyboard)

    zanna_add_test(test_rt_mouse ${ZANNA_TESTS_DIR}/runtime/RTMouseTests.cpp)
    target_link_libraries(test_rt_mouse PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_mouse test_rt_mouse)

    zanna_add_test(test_rt_pad ${ZANNA_TESTS_DIR}/runtime/RTPadTests.cpp)
    target_link_libraries(test_rt_pad PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_pad test_rt_pad)

    zanna_add_test(test_rt_abs_i64_overflow ${ZANNA_TESTS_DIR}/runtime/RTAbsI64OverflowTests.cpp)
    target_link_libraries(test_rt_abs_i64_overflow PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_abs_i64_overflow test_rt_abs_i64_overflow)

    zanna_add_test(test_rt_random ${ZANNA_TESTS_DIR}/runtime/RTRandomTests.cpp)
    target_link_libraries(test_rt_random PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_random test_rt_random)

    zanna_add_test(test_rt_zia_completion_stub ${ZANNA_TESTS_DIR}/runtime/RTZiaCompletionStubTests.cpp)
    target_link_libraries(test_rt_zia_completion_stub PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_zia_completion_stub test_rt_zia_completion_stub)

    zanna_add_test(test_rt_instr ${ZANNA_TESTS_DIR}/runtime/RTInstrTests.cpp)
    target_link_libraries(test_rt_instr PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_instr test_rt_instr)

    zanna_add_test(test_rt_term_color ${ZANNA_TESTS_DIR}/runtime/RTTermColorTests.cpp)
    target_link_libraries(test_rt_term_color PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    if (APPLE)
        target_link_libraries(test_rt_term_color PRIVATE util)
    endif ()
    zanna_add_ctest(test_rt_term_color test_rt_term_color)

    zanna_add_test(test_rt_trim_case ${ZANNA_TESTS_DIR}/runtime/RTTrimCaseTests.cpp)
    target_link_libraries(test_rt_trim_case PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_trim_case test_rt_trim_case)

    zanna_add_test(test_rt_chr_asc ${ZANNA_TESTS_DIR}/runtime/RTChrAscTests.cpp)
    target_link_libraries(test_rt_chr_asc PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_chr_asc test_rt_chr_asc)

    zanna_add_test(test_rt_chr_invalid ${ZANNA_TESTS_DIR}/runtime/RTChrInvalidTests.cpp)
    target_link_libraries(test_rt_chr_invalid PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} zanna_test_common)
    zanna_add_ctest(test_rt_chr_invalid test_rt_chr_invalid)

    # rt_args tests
    zanna_add_test(test_rt_args ${ZANNA_TESTS_DIR}/runtime/RTArgsTests.cpp)
    target_link_libraries(test_rt_args PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_args test_rt_args)

    zanna_add_test(test_rt_args_trap ${ZANNA_TESTS_DIR}/runtime/RTArgsTrapTests.cpp)
    target_link_libraries(test_rt_args_trap PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_args_trap test_rt_args_trap)

    # Sleep runtime test
    zanna_add_test(test_rt_sleep ${ZANNA_TESTS_DIR}/runtime/RT_SleepTests.cpp)
    target_link_libraries(test_rt_sleep PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_sleep test_rt_sleep)

    # Timer runtime test
    zanna_add_test(test_rt_timer ${ZANNA_TESTS_DIR}/runtime/RT_TimerTests.cpp)
    target_link_libraries(test_rt_timer PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_timer test_rt_timer)

    # Threading runtime tests (Monitor/Thread/SafeI64)
    zanna_add_test(test_rt_threads_monitor ${ZANNA_TESTS_DIR}/runtime/RTThreadsMonitorTests.cpp)
    target_link_libraries(test_rt_threads_monitor PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads zanna_test_common)
    zanna_add_ctest(test_rt_threads_monitor test_rt_threads_monitor)

    zanna_add_test(test_rt_threads_thread ${ZANNA_TESTS_DIR}/runtime/RTThreadsThreadTests.cpp)
    target_link_libraries(test_rt_threads_thread PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads zanna_test_common)
    zanna_add_ctest(test_rt_threads_thread test_rt_threads_thread)

    zanna_add_test(test_rt_threads_primitives ${ZANNA_TESTS_DIR}/runtime/RTThreadsPrimitivesTests.cpp)
    target_link_libraries(test_rt_threads_primitives PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads)
    zanna_add_ctest(test_rt_threads_primitives test_rt_threads_primitives)

    zanna_add_test(test_rt_threads_type_names ${ZANNA_TESTS_DIR}/runtime/RTThreadsTypeNameTests.cpp)
    target_link_libraries(test_rt_threads_type_names PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads)
    zanna_add_ctest(test_rt_threads_type_names test_rt_threads_type_names)

    zanna_add_test(test_rt_future ${ZANNA_TESTS_DIR}/runtime/RTFutureTests.cpp)
    target_link_libraries(test_rt_future PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads)
    zanna_add_ctest(test_rt_future test_rt_future)

    zanna_add_test(test_rt_parallel ${ZANNA_TESTS_DIR}/runtime/RTParallelTests.cpp)
    target_link_libraries(test_rt_parallel PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads zanna_test_common)
    zanna_add_ctest(test_rt_parallel test_rt_parallel)

    zanna_add_test(test_rt_parallel_reduce ${ZANNA_TESTS_DIR}/runtime/RTParallelReduceTests.cpp)
    target_link_libraries(test_rt_parallel_reduce PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads zanna_test_common)
    zanna_add_ctest(test_rt_parallel_reduce test_rt_parallel_reduce)

    zanna_add_test(test_rt_concmap ${ZANNA_TESTS_DIR}/runtime/RTConcMapTests.cpp)
    target_link_libraries(test_rt_concmap PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads)
    zanna_add_ctest(test_rt_concmap test_rt_concmap)

    zanna_add_test(test_rt_channel ${ZANNA_TESTS_DIR}/runtime/RTChannelTests.cpp)
    target_link_libraries(test_rt_channel PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads)
    zanna_add_ctest(test_rt_channel test_rt_channel)

    zanna_add_test(test_rt_threadpool ${ZANNA_TESTS_DIR}/runtime/RTThreadPoolTests.cpp)
    target_link_libraries(test_rt_threadpool PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads)
    zanna_add_ctest(test_rt_threadpool test_rt_threadpool)

    zanna_add_test(test_rt_handle_validation ${ZANNA_TESTS_DIR}/runtime/RTHandleValidationTests.cpp)
    target_link_libraries(test_rt_handle_validation PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads)
    zanna_add_ctest(test_rt_handle_validation test_rt_handle_validation)

    zanna_add_test(test_rt_concurrency ${ZANNA_TESTS_DIR}/runtime/RTConcurrencyTests.cpp)
    target_link_libraries(test_rt_concurrency PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads)
    zanna_add_ctest(test_rt_concurrency test_rt_concurrency)

    zanna_add_test(test_rt_async ${ZANNA_TESTS_DIR}/runtime/RTAsyncTests.cpp)
    target_link_libraries(test_rt_async PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads)
    zanna_add_ctest(test_rt_async test_rt_async)

    foreach (_timing_test
            test_rt_clock
            test_rt_stopwatch
            test_rt_timer
            test_rt_network
            test_rt_websocket
            test_rt_channel
            test_rt_threadpool
            test_rt_async)
        set_tests_properties(${_timing_test} PROPERTIES RUN_SERIAL TRUE)
    endforeach ()

    zanna_add_test(test_rt_serialize ${ZANNA_TESTS_DIR}/runtime/RTSerializeTests.cpp)
    target_link_libraries(test_rt_serialize PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_serialize test_rt_serialize)

    zanna_add_test(test_rt_yaml ${ZANNA_TESTS_DIR}/runtime/RTYamlTests.cpp)
    target_link_libraries(test_rt_yaml PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_yaml test_rt_yaml)

    zanna_add_test(test_rt_gc ${ZANNA_TESTS_DIR}/runtime/RTGCTests.cpp)
    target_link_libraries(test_rt_gc PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_gc test_rt_gc)

    zanna_add_test(test_rt_gc_hash ${ZANNA_TESTS_DIR}/runtime/RTGCHashTableTests.cpp)
    target_link_libraries(test_rt_gc_hash PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_gc_hash test_rt_gc_hash)

    zanna_add_test(test_rt_string_size_clamp ${ZANNA_TESTS_DIR}/runtime/RTStringSizeClampTests.cpp)
    target_link_libraries(test_rt_string_size_clamp PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_string_size_clamp test_rt_string_size_clamp)

    zanna_add_test(test_rt_string_ranges ${ZANNA_TESTS_DIR}/runtime/RTStringRangeTests.cpp)
    target_link_libraries(test_rt_string_ranges PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} zanna_test_common)
    zanna_add_ctest(test_rt_string_ranges test_rt_string_ranges)

    zanna_add_test(test_rt_alloc ${ZANNA_TESTS_DIR}/runtime/RTAllocTests.cpp)
    target_link_libraries(test_rt_alloc PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} zanna_test_common)
    zanna_add_ctest(test_rt_alloc test_rt_alloc)

    zanna_add_test(test_rt_alloc_oom ${ZANNA_TESTS_DIR}/runtime/RTAllocOOMTests.cpp)
    target_link_libraries(test_rt_alloc_oom PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_alloc_oom test_rt_alloc_oom)

    zanna_add_test(test_rt_alloc_too_large ${ZANNA_TESTS_DIR}/runtime/RTAllocTooLargeTests.cpp)
    target_link_libraries(test_rt_alloc_too_large PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} zanna_test_common)
    zanna_add_ctest(test_rt_alloc_too_large test_rt_alloc_too_large)

    zanna_add_test(
            test_rt_string_alloc_overflow
            ${ZANNA_TESTS_DIR}/runtime/RTStringAllocOverflowTests.cpp)
    target_link_libraries(test_rt_string_alloc_overflow PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} zanna_test_common)
    zanna_add_ctest(test_rt_string_alloc_overflow test_rt_string_alloc_overflow)

    zanna_add_test(
            test_rt_string_len_clamp
            ${ZANNA_TESTS_DIR}/runtime/RTStringLenClampTests.cpp)
    target_link_libraries(test_rt_string_len_clamp PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_string_len_clamp test_rt_string_len_clamp)

    zanna_add_test(
            test_rt_string_wrap_alloc_fail
            ${ZANNA_TESTS_DIR}/runtime/RTStringWrapAllocFailTests.cpp)
    target_link_libraries(test_rt_string_wrap_alloc_fail PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_string_wrap_alloc_fail test_rt_string_wrap_alloc_fail)

    zanna_add_test(test_rt_alloc_zero ${ZANNA_TESTS_DIR}/runtime/RTAllocZeroTests.cpp)
    target_link_libraries(test_rt_alloc_zero PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_alloc_zero test_rt_alloc_zero)

    zanna_add_test(test_rt_alloc_zero_fill ${ZANNA_TESTS_DIR}/runtime/RTAllocZeroFillTests.cpp)
    target_link_libraries(test_rt_alloc_zero_fill PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_alloc_zero_fill test_rt_alloc_zero_fill)

    zanna_add_test(test_rt_object_alloc ${ZANNA_TESTS_DIR}/runtime/RTObjectAllocTests.cpp)
    target_link_libraries(test_rt_object_alloc PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_object_alloc test_rt_object_alloc)

    zanna_add_test(test_rt_memory_surface ${ZANNA_TESTS_DIR}/runtime/RTMemorySurfaceTests.cpp)
    target_link_libraries(test_rt_memory_surface PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} zanna_test_common)
    zanna_add_ctest(test_rt_memory_surface test_rt_memory_surface)

    zanna_add_test(test_rt_oop_dispatch ${ZANNA_TESTS_DIR}/runtime/RTOOPDispatchTests.cpp)
    target_link_libraries(test_rt_oop_dispatch PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_oop_dispatch test_rt_oop_dispatch)

    zanna_add_test(test_rt_type_registry ${ZANNA_TESTS_DIR}/runtime/RTTypeRegistryTests.cpp)
    target_link_libraries(test_rt_type_registry PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_type_registry test_rt_type_registry)

    zanna_add_test(
            test_rt_type_registry_context_isolation
            ${ZANNA_TESTS_DIR}/runtime/RTTypeRegistryContextIsolationTests.cpp)
    target_link_libraries(test_rt_type_registry_context_isolation PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_type_registry_context_isolation test_rt_type_registry_context_isolation)

    zanna_add_test(test_rt_type_registry_thread_safety
            ${ZANNA_TESTS_DIR}/runtime/RTTypeRegistryThreadSafetyTests.cpp)
    target_link_libraries(test_rt_type_registry_thread_safety PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_type_registry_thread_safety test_rt_type_registry_thread_safety)

    zanna_add_test(test_rt_array_i32 ${ZANNA_TESTS_DIR}/runtime/RTArrayI32Tests.cpp)
    target_link_libraries(test_rt_array_i32 PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} zanna_test_common)
    zanna_add_ctest(test_rt_array_i32 test_rt_array_i32)

    zanna_add_test(test_rt_array_i32_refcount ${ZANNA_TESTS_DIR}/runtime/RTArrayI32RefcountTests.cpp)
    target_link_libraries(test_rt_array_i32_refcount PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_array_i32_refcount test_rt_array_i32_refcount)

    zanna_add_test(test_rt_array_str ${ZANNA_TESTS_DIR}/runtime/RTArrayStrTests.cpp)
    target_link_libraries(test_rt_array_str PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_array_str test_rt_array_str)

    zanna_add_test(test_rt_input_line ${ZANNA_TESTS_DIR}/runtime/RTInputLineTests.cpp)
    target_link_libraries(test_rt_input_line PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_input_line test_rt_input_line)

    zanna_add_test(test_rt_input_overflow ${ZANNA_TESTS_DIR}/unit/runtime/RTInputGrowOverflowTests.cpp)
    target_link_libraries(test_rt_input_overflow PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_input_overflow test_rt_input_overflow)

    zanna_add_test(
            test_rt_file_read_line_overflow
            ${ZANNA_TESTS_DIR}/unit/runtime/RTFileReadLineOverflowTests.cpp)
    target_link_libraries(test_rt_file_read_line_overflow PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_file_read_line_overflow test_rt_file_read_line_overflow)

    zanna_add_test(test_rt_action_mapping ${ZANNA_TESTS_DIR}/unit/runtime/RTActionMappingTests.cpp)
    target_link_libraries(test_rt_action_mapping PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_action_mapping test_rt_action_mapping)

    zanna_add_test(test_rt_keychord ${ZANNA_TESTS_DIR}/runtime/RTKeyChordTests.cpp)
    target_link_libraries(test_rt_keychord PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_keychord test_rt_keychord)

    zanna_add_test(test_rt_action_persist ${ZANNA_TESTS_DIR}/runtime/RTActionPersistTests.cpp)
    target_link_libraries(test_rt_action_persist PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_action_persist test_rt_action_persist)

    zanna_add_test(test_rt_grid2d ${ZANNA_TESTS_DIR}/unit/runtime/RTGrid2DTests.cpp)
    target_link_libraries(test_rt_grid2d PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_grid2d test_rt_grid2d)

    zanna_add_test(test_rt_frame_timer ${ZANNA_TESTS_DIR}/unit/runtime/RTTimerTests.cpp)
    target_link_libraries(test_rt_frame_timer PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_frame_timer test_rt_frame_timer)

    zanna_add_test(test_rt_input_line_fail ${ZANNA_TESTS_DIR}/runtime/RTInputLineFailTests.cpp)
    target_link_libraries(test_rt_input_line_fail PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_input_line_fail test_rt_input_line_fail)

    zanna_add_test(test_rt_input_numeric_fail ${ZANNA_TESTS_DIR}/runtime/RTInputNumericFailTests.cpp)
    target_link_libraries(test_rt_input_numeric_fail PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} zanna_test_common)
    zanna_add_ctest(test_rt_input_numeric_fail test_rt_input_numeric_fail)

    # Basic SLEEP lowering test
    if (NOT TARGET test_basic_sleep_lowering)
        zanna_add_test(test_basic_sleep_lowering ${ZANNA_TESTS_DIR}/unit/Basic_Sleep_Lowering.cpp)
        target_link_libraries(test_basic_sleep_lowering PRIVATE fe_basic)
        zanna_add_ctest(test_basic_sleep_lowering test_basic_sleep_lowering)
    endif ()

    # Basic TIMER lowering test
    if (NOT TARGET test_basic_timer_lowering)
        zanna_add_test(test_basic_timer_lowering ${ZANNA_TESTS_DIR}/unit/Basic_Timer_Lowering.cpp)
        target_link_libraries(test_basic_timer_lowering PRIVATE fe_basic)
        zanna_add_ctest(test_basic_timer_lowering test_basic_timer_lowering)
    endif ()

    zanna_add_test(test_rt_split_fields ${ZANNA_TESTS_DIR}/runtime/RTSplitFieldsTests.cpp)
    target_link_libraries(test_rt_split_fields PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_split_fields test_rt_split_fields)

    zanna_add_test(test_rt_seq ${ZANNA_TESTS_DIR}/runtime/RTSeqTests.cpp)
    target_link_libraries(test_rt_seq PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_seq test_rt_seq)

    zanna_add_test(test_rt_list ${ZANNA_TESTS_DIR}/runtime/RTListTests.cpp)
    target_link_libraries(test_rt_list PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_list test_rt_list)

    zanna_add_test(test_rt_stack ${ZANNA_TESTS_DIR}/runtime/RTStackTests.cpp)
    target_link_libraries(test_rt_stack PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_stack test_rt_stack)

    zanna_add_test(test_rt_queue ${ZANNA_TESTS_DIR}/runtime/RTQueueTests.cpp)
    target_link_libraries(test_rt_queue PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_queue test_rt_queue)

    zanna_add_test(test_rt_ring ${ZANNA_TESTS_DIR}/runtime/RTRingTests.cpp)
    target_link_libraries(test_rt_ring PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_ring test_rt_ring)

    zanna_add_test(test_rt_bitset ${ZANNA_TESTS_DIR}/runtime/RTBitSetTests.cpp)
    target_link_libraries(test_rt_bitset PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_bitset test_rt_bitset)

    zanna_add_test(test_rt_lrucache ${ZANNA_TESTS_DIR}/runtime/RTLruCacheTests.cpp)
    target_link_libraries(test_rt_lrucache PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_lrucache test_rt_lrucache)

    zanna_add_test(test_rt_map ${ZANNA_TESTS_DIR}/runtime/RTMapTests.cpp)
    target_link_libraries(test_rt_map PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_map test_rt_map)

    zanna_add_test(test_rt_intmap ${ZANNA_TESTS_DIR}/runtime/RTIntMapTests.cpp)
    target_link_libraries(test_rt_intmap PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_intmap test_rt_intmap)

    zanna_add_test(test_rt_multimap ${ZANNA_TESTS_DIR}/runtime/RTMultiMapTests.cpp)
    target_link_libraries(test_rt_multimap PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_multimap test_rt_multimap)

    zanna_add_test(test_rt_treemap ${ZANNA_TESTS_DIR}/runtime/RTTreeMapTests.cpp)
    target_link_libraries(test_rt_treemap PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_treemap test_rt_treemap)

    zanna_add_test(test_rt_trie ${ZANNA_TESTS_DIR}/runtime/RTTrieTests.cpp)
    target_link_libraries(test_rt_trie PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_trie test_rt_trie)

    zanna_add_test(test_rt_modvar ${ZANNA_TESTS_DIR}/runtime/RTModvarTests.cpp)
    target_link_libraries(test_rt_modvar PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_modvar test_rt_modvar)

    zanna_add_test(test_rt_modvar_contracts ${ZANNA_TESTS_DIR}/runtime/RTModvarContractTests.cpp)
    target_link_libraries(test_rt_modvar_contracts PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_modvar_contracts test_rt_modvar_contracts)

    zanna_add_test(test_rt_bytes ${ZANNA_TESTS_DIR}/runtime/RTBytesTests.cpp)
    target_link_libraries(test_rt_bytes PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_bytes test_rt_bytes)

    zanna_add_test(test_rt_string_ext ${ZANNA_TESTS_DIR}/runtime/RTStringExtTests.cpp)
    target_link_libraries(test_rt_string_ext PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_string_ext test_rt_string_ext)

    zanna_add_test(test_rt_string_edge ${ZANNA_TESTS_DIR}/runtime/RTStringEdgeTests.cpp)
    target_link_libraries(test_rt_string_edge PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_string_edge test_rt_string_edge)

    # Game development abstractions tests
    zanna_add_test(test_rt_statemachine ${ZANNA_TESTS_DIR}/unit/runtime/RTStateMachineTests.cpp)
    target_link_libraries(test_rt_statemachine PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_statemachine test_rt_statemachine)

    zanna_add_test(test_rt_tween ${ZANNA_TESTS_DIR}/unit/runtime/RTTweenTests.cpp)
    target_link_libraries(test_rt_tween PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_tween test_rt_tween)

    zanna_add_test(test_rt_buttongroup ${ZANNA_TESTS_DIR}/unit/runtime/RTButtonGroupTests.cpp)
    target_link_libraries(test_rt_buttongroup PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_buttongroup test_rt_buttongroup)

    zanna_add_test(test_rt_debugoverlay ${ZANNA_TESTS_DIR}/runtime/RTDebugOverlayTests.cpp)
    target_link_libraries(test_rt_debugoverlay PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_debugoverlay test_rt_debugoverlay)

    zanna_add_test(test_rt_config ${ZANNA_TESTS_DIR}/unit/runtime/TestConfig.cpp)
    target_link_libraries(test_rt_config PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_config test_rt_config)

    zanna_add_test(test_rt_quests ${ZANNA_TESTS_DIR}/unit/runtime/TestQuests.cpp)
    target_link_libraries(test_rt_quests PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_quests test_rt_quests)

    zanna_add_test(test_rt_leveldata ${ZANNA_TESTS_DIR}/unit/runtime/TestLevelData.cpp)
    target_link_libraries(test_rt_leveldata PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_leveldata test_rt_leveldata)

    zanna_add_test(test_rt_soundbank ${ZANNA_TESTS_DIR}/runtime/RTSoundBankTests.cpp)
    target_link_libraries(test_rt_soundbank PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_soundbank test_rt_soundbank)

    zanna_add_test(test_rt_musicgen ${ZANNA_TESTS_DIR}/runtime/RTMusicGenTests.cpp)
    target_link_libraries(test_rt_musicgen PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_musicgen test_rt_musicgen)

    zanna_add_test(test_rt_camera ${ZANNA_TESTS_DIR}/unit/runtime/RTCameraTests.cpp)
    target_link_libraries(test_rt_camera PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_camera test_rt_camera)

    zanna_add_test(test_rt_smoothvalue ${ZANNA_TESTS_DIR}/unit/runtime/RTSmoothValueTests.cpp)
    target_link_libraries(test_rt_smoothvalue PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_smoothvalue test_rt_smoothvalue)

    zanna_add_test(test_rt_particle ${ZANNA_TESTS_DIR}/unit/runtime/RTParticleTests.cpp)
    target_link_libraries(test_rt_particle PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_particle test_rt_particle)

    zanna_add_test(test_rt_spriteanim ${ZANNA_TESTS_DIR}/unit/runtime/RTSpriteAnimTests.cpp)
    target_link_libraries(test_rt_spriteanim PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_spriteanim test_rt_spriteanim)

    zanna_add_test(test_rt_animstate ${ZANNA_TESTS_DIR}/runtime/RTAnimStateTests.cpp)
    target_link_libraries(test_rt_animstate PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_animstate test_rt_animstate)

    zanna_add_test(test_rt_collision ${ZANNA_TESTS_DIR}/unit/runtime/RTCollisionTests.cpp)
    target_link_libraries(test_rt_collision PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_collision test_rt_collision)

    zanna_add_test(test_rt_objpool ${ZANNA_TESTS_DIR}/unit/runtime/RTObjPoolTests.cpp)
    target_link_libraries(test_rt_objpool PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_objpool test_rt_objpool)

    zanna_add_test(test_rt_screenfx ${ZANNA_TESTS_DIR}/unit/runtime/RTScreenFXTests.cpp)
    target_link_libraries(test_rt_screenfx PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_screenfx test_rt_screenfx)

    zanna_add_test(test_rt_pathfollow ${ZANNA_TESTS_DIR}/unit/runtime/RTPathFollowTests.cpp)
    target_link_libraries(test_rt_pathfollow PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_pathfollow test_rt_pathfollow)

    zanna_add_test(test_rt_quadtree ${ZANNA_TESTS_DIR}/unit/runtime/RTQuadtreeTests.cpp)
    target_link_libraries(test_rt_quadtree PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_quadtree test_rt_quadtree)

    zanna_add_test(test_rt_perlin ${ZANNA_TESTS_DIR}/runtime/RTPerlinTests.cpp)
    target_link_libraries(test_rt_perlin PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_perlin test_rt_perlin)

    zanna_add_test(test_rt_easing ${ZANNA_TESTS_DIR}/runtime/RTEasingTests.cpp)
    target_link_libraries(test_rt_easing PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_easing test_rt_easing)

    zanna_add_test(test_rt_color_utils ${ZANNA_TESTS_DIR}/runtime/RTColorUtilsTests.cpp)
    target_link_libraries(test_rt_color_utils PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_color_utils test_rt_color_utils)

    zanna_add_test(test_rt_ini ${ZANNA_TESTS_DIR}/runtime/RTIniTests.cpp)
    target_link_libraries(test_rt_ini PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_ini test_rt_ini)

    zanna_add_test(test_rt_fmt_ext ${ZANNA_TESTS_DIR}/runtime/RTFmtExtTests.cpp)
    target_link_libraries(test_rt_fmt_ext PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_fmt_ext test_rt_fmt_ext)

    zanna_add_test(test_rt_case_convert ${ZANNA_TESTS_DIR}/runtime/RTCaseConvertTests.cpp)
    target_link_libraries(test_rt_case_convert PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_case_convert test_rt_case_convert)

    zanna_add_test(test_rt_bimap ${ZANNA_TESTS_DIR}/runtime/RTBiMapTests.cpp)
    target_link_libraries(test_rt_bimap PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_bimap test_rt_bimap)

    zanna_add_test(test_rt_countmap ${ZANNA_TESTS_DIR}/runtime/RTCountMapTests.cpp)
    target_link_libraries(test_rt_countmap PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_countmap test_rt_countmap)

    zanna_add_test(test_rt_pluralize ${ZANNA_TESTS_DIR}/runtime/RTPluralizeTests.cpp)
    target_link_libraries(test_rt_pluralize PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_pluralize test_rt_pluralize)

    zanna_add_test(test_rt_version ${ZANNA_TESTS_DIR}/runtime/RTVersionTests.cpp)
    target_link_libraries(test_rt_version PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_version test_rt_version)

    zanna_add_test(test_rt_reltime ${ZANNA_TESTS_DIR}/runtime/RTRelTimeTests.cpp)
    target_link_libraries(test_rt_reltime PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_reltime test_rt_reltime)

    zanna_add_test(test_rt_daterange ${ZANNA_TESTS_DIR}/runtime/RTDateRangeTests.cpp)
    target_link_libraries(test_rt_daterange PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_daterange test_rt_daterange)

    zanna_add_test(test_rt_numfmt ${ZANNA_TESTS_DIR}/runtime/RTNumFmtTests.cpp)
    target_link_libraries(test_rt_numfmt PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_numfmt test_rt_numfmt)

    zanna_add_test(test_rt_bloomfilter ${ZANNA_TESTS_DIR}/runtime/RTBloomFilterTests.cpp)
    target_link_libraries(test_rt_bloomfilter PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_bloomfilter test_rt_bloomfilter)

    zanna_add_test(test_rt_unionfind ${ZANNA_TESTS_DIR}/runtime/RTUnionFindTests.cpp)
    target_link_libraries(test_rt_unionfind PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_unionfind test_rt_unionfind)

    zanna_add_test(test_rt_orderedmap ${ZANNA_TESTS_DIR}/runtime/RTOrderedMapTests.cpp)
    target_link_libraries(test_rt_orderedmap PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_orderedmap test_rt_orderedmap)

    zanna_add_test(test_rt_diff ${ZANNA_TESTS_DIR}/runtime/RTDiffTests.cpp)
    target_link_libraries(test_rt_diff PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_diff test_rt_diff)

    zanna_add_test(test_rt_defaultmap ${ZANNA_TESTS_DIR}/runtime/RTDefaultMapTests.cpp)
    target_link_libraries(test_rt_defaultmap PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_defaultmap test_rt_defaultmap)

    zanna_add_test(test_rt_frozenset ${ZANNA_TESTS_DIR}/runtime/RTFrozenSetTests.cpp)
    target_link_libraries(test_rt_frozenset PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_frozenset test_rt_frozenset)

    zanna_add_test(test_rt_frozenmap ${ZANNA_TESTS_DIR}/runtime/RTFrozenMapTests.cpp)
    target_link_libraries(test_rt_frozenmap PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_frozenmap test_rt_frozenmap)

    zanna_add_test(test_rt_sparsearray ${ZANNA_TESTS_DIR}/runtime/RTSparseArrayTests.cpp)
    target_link_libraries(test_rt_sparsearray PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_sparsearray test_rt_sparsearray)

    zanna_add_test(test_rt_concqueue ${ZANNA_TESTS_DIR}/runtime/RTConcQueueTests.cpp)
    target_link_libraries(test_rt_concqueue PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads)
    zanna_add_ctest(test_rt_concqueue test_rt_concqueue)

    zanna_add_test(test_rt_msgbus ${ZANNA_TESTS_DIR}/runtime/RTMsgBusTests.cpp)
    target_link_libraries(test_rt_msgbus PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_msgbus test_rt_msgbus)

    zanna_add_test(test_rt_toml ${ZANNA_TESTS_DIR}/runtime/RTTomlTests.cpp)
    target_link_libraries(test_rt_toml PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_toml test_rt_toml)

    zanna_add_test(test_rt_markdown ${ZANNA_TESTS_DIR}/runtime/RTMarkdownTests.cpp)
    target_link_libraries(test_rt_markdown PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_markdown test_rt_markdown)

    zanna_add_test(test_rt_cancellation ${ZANNA_TESTS_DIR}/runtime/RTCancellationTests.cpp)
    target_link_libraries(test_rt_cancellation PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_cancellation test_rt_cancellation)

    zanna_add_test(test_rt_jsonpath ${ZANNA_TESTS_DIR}/runtime/RTJsonPathTests.cpp)
    target_link_libraries(test_rt_jsonpath PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_jsonpath test_rt_jsonpath)

    zanna_add_test(test_rt_debounce ${ZANNA_TESTS_DIR}/runtime/RTDebounceTests.cpp)
    target_link_libraries(test_rt_debounce PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads)
    zanna_add_ctest(test_rt_debounce test_rt_debounce)

    zanna_add_test(test_rt_retry ${ZANNA_TESTS_DIR}/runtime/RTRetryTests.cpp)
    target_link_libraries(test_rt_retry PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_retry test_rt_retry)

    zanna_add_test(test_rt_binbuf ${ZANNA_TESTS_DIR}/runtime/RTBinaryBufferTests.cpp)
    target_link_libraries(test_rt_binbuf PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_binbuf test_rt_binbuf)

    zanna_add_test(test_rt_weakmap ${ZANNA_TESTS_DIR}/runtime/RTWeakMapTests.cpp)
    target_link_libraries(test_rt_weakmap PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_weakmap test_rt_weakmap)

    zanna_add_test(test_rt_scheduler ${ZANNA_TESTS_DIR}/runtime/RTSchedulerTests.cpp)
    target_link_libraries(test_rt_scheduler PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads)
    zanna_add_ctest(test_rt_scheduler test_rt_scheduler)

    zanna_add_test(test_rt_ratelimit ${ZANNA_TESTS_DIR}/runtime/RTRateLimitTests.cpp)
    target_link_libraries(test_rt_ratelimit PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_ratelimit test_rt_ratelimit)

    zanna_add_test(test_rt_html ${ZANNA_TESTS_DIR}/runtime/RTHtmlTests.cpp)
    target_link_libraries(test_rt_html PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_html test_rt_html)

    zanna_add_test(test_rt_iter ${ZANNA_TESTS_DIR}/runtime/RTIterTests.cpp)
    target_link_libraries(test_rt_iter PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_iter test_rt_iter)

    ## Removed: test_rt_spritesheet (consolidated into test_rt_sprite_consolidated)

    zanna_add_test(test_rt_physics2d ${ZANNA_TESTS_DIR}/runtime/RTPhysics2DTests.cpp)
    target_link_libraries(test_rt_physics2d PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_physics2d test_rt_physics2d)

    zanna_add_test(test_rt_audio_integration ${ZANNA_TESTS_DIR}/runtime/RTAudioIntegrationTests.cpp)
    target_link_libraries(test_rt_audio_integration PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} zanna_packaging)
    zanna_add_ctest(test_rt_audio_integration test_rt_audio_integration)

    zanna_add_test(test_rt_audio_fx ${ZANNA_TESTS_DIR}/runtime/RTAudioFxTests.cpp)
    target_link_libraries(test_rt_audio_fx PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_audio_fx test_rt_audio_fx)

    zanna_add_test(test_rt_heap_alloc ${ZANNA_TESTS_DIR}/runtime/RTHeapAllocTests.cpp)
    target_link_libraries(test_rt_heap_alloc PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads)
    zanna_add_ctest(test_rt_heap_alloc test_rt_heap_alloc)

    zanna_add_test(test_rt_object_introspect ${ZANNA_TESTS_DIR}/runtime/RTObjectIntrospectTests.cpp)
    target_link_libraries(test_rt_object_introspect PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_object_introspect test_rt_object_introspect)

    zanna_add_test(test_rt_arithmetic_ub ${ZANNA_TESTS_DIR}/runtime/RTArithmeticUBTests.c)
    target_link_libraries(test_rt_arithmetic_ub PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_arithmetic_ub test_rt_arithmetic_ub)

    zanna_add_test(test_rt_buffer_overflow ${ZANNA_TESTS_DIR}/runtime/RTBufferOverflowTests.c)
    target_link_libraries(test_rt_buffer_overflow PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_buffer_overflow test_rt_buffer_overflow)

    zanna_add_test(test_rt_concurrency_bugs ${ZANNA_TESTS_DIR}/runtime/RTConcurrencyTests.c)
    target_link_libraries(test_rt_concurrency_bugs PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} Threads::Threads)
    zanna_add_ctest(test_rt_concurrency_bugs test_rt_concurrency_bugs)

    zanna_add_test(test_rt_graphics_mem ${ZANNA_TESTS_DIR}/runtime/RTGraphicsMemTests.c)
    target_link_libraries(test_rt_graphics_mem PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_graphics_mem test_rt_graphics_mem)

    zanna_add_test(test_rt_null_checks ${ZANNA_TESTS_DIR}/runtime/RTNullCheckTests.c)
    target_link_libraries(test_rt_null_checks PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_null_checks test_rt_null_checks)

    zanna_add_test(test_rt_realloc_mem ${ZANNA_TESTS_DIR}/runtime/RTReallocMemTests.c)
    target_link_libraries(test_rt_realloc_mem PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_realloc_mem test_rt_realloc_mem)

    zanna_add_test(test_rt_security_fixes ${ZANNA_TESTS_DIR}/runtime/RTSecurityFixesTests.c)
    target_link_libraries(test_rt_security_fixes PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_security_fixes test_rt_security_fixes)

    zanna_add_test(test_rt_optimization_fixes ${ZANNA_TESTS_DIR}/runtime/RTOptimizationFixesTests.c)
    target_link_libraries(test_rt_optimization_fixes PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_optimization_fixes test_rt_optimization_fixes)

    zanna_add_test(test_rt_network_runtime ${ZANNA_TESTS_DIR}/runtime/RTNetworkRuntimeTests.c)
    target_link_libraries(test_rt_network_runtime PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_network_runtime test_rt_network_runtime)

    # P2 optimisation tests
    zanna_add_test(test_rt_vec2_pool ${ZANNA_TESTS_DIR}/runtime/RTVec2PoolTests.cpp)
    target_link_libraries(test_rt_vec2_pool PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_vec2_pool test_rt_vec2_pool)

    zanna_add_test(test_rt_vec3_pool ${ZANNA_TESTS_DIR}/runtime/RTVec3PoolTests.cpp)
    target_link_libraries(test_rt_vec3_pool PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_vec3_pool test_rt_vec3_pool)

    zanna_add_test(test_rt_list_i64 ${ZANNA_TESTS_DIR}/runtime/RTListI64Tests.cpp)
    target_link_libraries(test_rt_list_i64 PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_list_i64 test_rt_list_i64)

    zanna_add_test(test_rt_string_intern ${ZANNA_TESTS_DIR}/runtime/RTStringInternTests.cpp)
    target_link_libraries(test_rt_string_intern PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_string_intern test_rt_string_intern)

    zanna_add_test(test_rt_thread_safety ${ZANNA_TESTS_DIR}/runtime/RTThreadSafetyTests.cpp)
    target_link_libraries(test_rt_thread_safety PRIVATE ${ZANNA_RUNTIME_TEST_LIBS} il_runtime)
    zanna_add_ctest(test_rt_thread_safety test_rt_thread_safety)

    # Resource leak detection tests
    zanna_add_test(test_rt_resource_leak ${ZANNA_TESTS_DIR}/runtime/RTResourceLeakTests.cpp)
    target_link_libraries(test_rt_resource_leak PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_resource_leak test_rt_resource_leak)

    # BASIC parser recursion depth limit tests
    if (NOT TARGET test_basic_depth_limits)
        zanna_add_test(test_basic_depth_limits ${ZANNA_TESTS_DIR}/unit/test_basic_depth_limits.cpp)
        target_link_libraries(test_basic_depth_limits PRIVATE fe_basic)
        if (WIN32)
            target_link_options(test_basic_depth_limits PRIVATE /STACK:8388608)
        endif ()
        zanna_add_ctest(test_basic_depth_limits test_basic_depth_limits)
    endif ()

    zanna_add_test(test_source_loader_limits ${ZANNA_TESTS_DIR}/unit/test_source_loader_limits.cpp)
    target_link_libraries(test_source_loader_limits PRIVATE il_tools_common zanna_test_common)
    zanna_add_ctest(test_source_loader_limits test_source_loader_limits)

    # Shutdown path resource cleanup tests
    zanna_add_test(test_rt_shutdown ${ZANNA_TESTS_DIR}/runtime/RTShutdownTests.cpp)
    target_link_libraries(test_rt_shutdown PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_shutdown test_rt_shutdown)

    # HKDF / secure-zero correctness tests (Phase 1 Item 3)
    zanna_add_test(test_rt_hkdf ${ZANNA_TESTS_DIR}/runtime/RTHkdfTests.cpp)
    target_link_libraries(test_rt_hkdf PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_hkdf test_rt_hkdf)

    # String UTF-8 Mid tests (Phase 2 Item 9)
    zanna_add_test(test_rt_string_utf8_mid ${ZANNA_TESTS_DIR}/runtime/RTStringUtf8MidTests.cpp)
    target_link_libraries(test_rt_string_utf8_mid PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_string_utf8_mid test_rt_string_utf8_mid)

    # GC batch lock correctness tests (Phase 2 Item 13)
    zanna_add_test(test_rt_gc_batch_lock ${ZANNA_TESTS_DIR}/runtime/RTGCBatchLockTests.cpp)
    target_link_libraries(test_rt_gc_batch_lock PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_gc_batch_lock test_rt_gc_batch_lock)

    # Comprehensive rt_map tests (Phase 3 Item 19)
    zanna_add_test(test_rt_map_comprehensive ${ZANNA_TESTS_DIR}/runtime/RTMapComprehensiveTests.cpp)
    target_link_libraries(test_rt_map_comprehensive PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_map_comprehensive test_rt_map_comprehensive)

    # rt_pool allocator tests (Phase 3 Item 19)
    zanna_add_test(test_rt_pool ${ZANNA_TESTS_DIR}/runtime/RTPoolTests.cpp)
    target_link_libraries(test_rt_pool PRIVATE ${ZANNA_RUNTIME_TEST_LIBS})
    zanna_add_ctest(test_rt_pool test_rt_pool)
endfunction()
