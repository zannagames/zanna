#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: src/tests/tools/CMakeLists.txt
# Purpose: Register CLI, packaging, and tool-specific smoke tests.
#
# Key invariants: Platform-specific tests are labeled and bounded.
#
# Ownership/Lifetime: CTest owns generated test workspaces in the build tree.
#
# Links: docs/internals/testing.md, src/tests/CMakeLists.txt
#
#===----------------------------------------------------------------------===#

set(ZANNA_TOOLS_LIBS zanna_il_full il_vm il_api il_transform il_tools_common zanna_bytecode)
set(ZANNA_TOOLS_LIBS ${ZANNA_TOOLS_LIBS} PARENT_SCOPE)

function(zanna_add_tools_tests)
    if (TARGET test_tools_break_parsing)
        return()
    endif ()

    set(_ZANNA_TOOLS_DIR ${CMAKE_CURRENT_FUNCTION_LIST_DIR})

    zanna_add_test(test_tools_break_parsing
            ${_ZANNA_TOOLS_DIR}/BreakParsingTests.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/break_spec.cpp)
    target_include_directories(test_tools_break_parsing PRIVATE ${CMAKE_SOURCE_DIR}/src)
    zanna_add_ctest(test_tools_break_parsing test_tools_break_parsing)

    zanna_add_test(test_tools_module_loader
            ${_ZANNA_TOOLS_DIR}/ModuleLoaderTests.cpp)
    target_include_directories(test_tools_module_loader PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(test_tools_module_loader PRIVATE ZANNA_REPO_ROOT="${CMAKE_SOURCE_DIR}")
    target_link_libraries(test_tools_module_loader PRIVATE ${ZANNA_TOOLS_LIBS})
    zanna_add_ctest(test_tools_module_loader test_tools_module_loader)

    zanna_add_test(test_tools_basic_common
            ${_ZANNA_TOOLS_DIR}/BasicCommonTests.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/basic/common.cpp)
    target_include_directories(test_tools_basic_common PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(test_tools_basic_common PRIVATE ZANNA_BASIC_TOOL_USAGE="Usage: basic-tool <file.bas>\\n")
    target_link_libraries(test_tools_basic_common PRIVATE ${ZANNA_TOOLS_LIBS})
    zanna_add_ctest(test_tools_basic_common test_tools_basic_common)

    zanna_add_ctest(diagnostic_cli
            ${CMAKE_COMMAND}
            -DZANNA_EXE=$<TARGET_FILE:zanna>
            -DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/diagnostic_cli
            -P ${_ZANNA_TOOLS_DIR}/DiagnosticCliTests.cmake)
    zanna_add_test_labels(diagnostic_cli tools zia)

    zanna_add_ctest(cli_help
            ${CMAKE_COMMAND}
            -DZANNA_EXE=$<TARGET_FILE:zanna>
            -P ${_ZANNA_TOOLS_DIR}/CliHelpTests.cmake)
    zanna_add_test_labels(cli_help tools)

    zanna_add_ctest(agent_cli
            ${CMAKE_COMMAND}
            -DZANNA_EXE=$<TARGET_FILE:zanna>
            -DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/agent_cli
            -P ${_ZANNA_TOOLS_DIR}/AgentCliTests.cmake)
    zanna_add_test_labels(agent_cli tools zia)

    zanna_add_ctest(rtgen_definition_manifest
            ${CMAKE_COMMAND}
            -DRTGEN_EXE=$<TARGET_FILE:rtgen>
            -DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/rtgen_definition_manifest
            -P ${_ZANNA_TOOLS_DIR}/RtgenDefinitionManifestTests.cmake)
    zanna_add_test_labels(rtgen_definition_manifest tools runtime)

    zanna_add_ctest(runtime_reference_docs
            $<TARGET_FILE:rtgen>
            --docs --check
            ${CMAKE_SOURCE_DIR}/src/il/runtime/runtime.def
            ${CMAKE_SOURCE_DIR}/docs/generated/runtime)
    zanna_add_test_labels(runtime_reference_docs tools runtime contract)

    zanna_add_test(test_tools_frontend_native_compiler
            ${_ZANNA_TOOLS_DIR}/FrontendToolAndNativeCompilerTests.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/cmd_codegen_x64.cpp)
    target_include_directories(test_tools_frontend_native_compiler PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_tools_frontend_native_compiler PRIVATE ${ZANNA_TOOLS_LIBS} zanna_native_compiler zanna_codegen_x86_64)
    zanna_add_ctest(test_tools_frontend_native_compiler test_tools_frontend_native_compiler)

    zanna_add_test(test_tools_asset_compiler
            ${_ZANNA_TOOLS_DIR}/AssetCompilerTests.cpp)
    target_include_directories(test_tools_asset_compiler PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(test_tools_asset_compiler PRIVATE ZANNA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(test_tools_asset_compiler PRIVATE ${ZANNA_TOOLS_LIBS} zanna_packaging)
    zanna_add_ctest(test_tools_asset_compiler test_tools_asset_compiler)

    zanna_add_ctest(run_build_performance_flags
            ${CMAKE_COMMAND}
            -DZANNA_EXE=$<TARGET_FILE:zanna>
            -DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/run_build_performance_flags
            -P ${_ZANNA_TOOLS_DIR}/RunBuildPerformanceFlagTests.cmake)
    zanna_add_test_labels(run_build_performance_flags tools zia)

    zanna_add_ctest(package_cli
            ${CMAKE_COMMAND}
            -DZANNA_BIN=$<TARGET_FILE:zanna>
            -DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/package_cli
            -P ${_ZANNA_TOOLS_DIR}/PackageCliTests.cmake)
    zanna_add_test_labels(package_cli tools installer)

    if (APPLE)
        if (ZANNA_INSTALL_ZANNASTUDIO)
            zanna_add_ctest(zannastudio_macos_staging
                    ${CMAKE_COMMAND}
                    -DZANNASTUDIO_LAUNCHER=${CMAKE_BINARY_DIR}/zannastudio/zannastudio
                    "-DZANNASTUDIO_NATIVE=${CMAKE_BINARY_DIR}/zannastudio/Zanna Studio"
                    -DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/zannastudio_macos_staging
                    -P ${_ZANNA_TOOLS_DIR}/ZannaStudioMacStagingTests.cmake)
            zanna_add_test_labels(zannastudio_macos_staging
                    tools installer requires_macos)
        endif ()

        zanna_add_ctest(package_crackman_macos_signed
                ${CMAKE_COMMAND}
                -DZANNA_BIN=$<TARGET_FILE:zanna>
                -DZANNA_REPO_ROOT=${CMAKE_SOURCE_DIR}
                -DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/package_crackman_macos_signed
                -P ${_ZANNA_TOOLS_DIR}/MacOSPackageSmokeTests.cmake)
        zanna_add_test_labels(package_crackman_macos_signed tools installer zia slow)

        zanna_add_ctest(macos_toolchain_installer_smoke
                ${CMAKE_COMMAND}
                -DCMAKE_BIN=${CMAKE_COMMAND}
                -DZANNA_BIN=$<TARGET_FILE:zanna>
                -DZANNA_BUILD_DIR=${CMAKE_BINARY_DIR}
                -DZANNA_CONFIG=$<CONFIG>
                -P ${_ZANNA_TOOLS_DIR}/MacOSToolchainInstallerSmoke.cmake)
        set_tests_properties(macos_toolchain_installer_smoke PROPERTIES
                RUN_SERIAL TRUE
                TIMEOUT 900
                SKIP_REGULAR_EXPRESSION "Skipping macOS installer smoke")
        zanna_add_test_labels(macos_toolchain_installer_smoke installer tools requires_macos requires_privileged_install slow)
    endif ()

    add_test(NAME zia_project_template_smoke
            COMMAND ${CMAKE_COMMAND}
            -DZIA_BIN=$<TARGET_FILE:zia>
            -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DWORK_DIR=${CMAKE_BINARY_DIR}/template-smoke
            -P ${_ZANNA_TOOLS_DIR}/ProjectTemplateSmoke.cmake)
    set_tests_properties(zia_project_template_smoke PROPERTIES
            TIMEOUT 240)
    zanna_add_test_labels(zia_project_template_smoke zia tools)

    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        zanna_add_ctest(linux_headless_graphics_smoke
                ${CMAKE_COMMAND}
                -DCMAKE_BIN=${CMAKE_COMMAND}
                -DZANNA_SOURCE_DIR=${CMAKE_SOURCE_DIR}
                -DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/linux_headless_graphics_smoke
                -P ${_ZANNA_TOOLS_DIR}/HeadlessGraphicsSmoke.cmake)
        set_tests_properties(linux_headless_graphics_smoke PROPERTIES
                RUN_SERIAL TRUE
                TIMEOUT 180)
        zanna_add_test_labels(linux_headless_graphics_smoke gui smoke tools requires_linux)

        zanna_add_ctest(linux_toolchain_deb_installer_smoke
                ${CMAKE_COMMAND}
                -DCMAKE_BIN=${CMAKE_COMMAND}
                -DZANNA_BIN=$<TARGET_FILE:zanna>
                -DZANNA_BUILD_DIR=${CMAKE_BINARY_DIR}
                -DZANNA_CONFIG=$<CONFIG>
                -P ${_ZANNA_TOOLS_DIR}/LinuxToolchainDebInstallerSmoke.cmake)
        set_tests_properties(linux_toolchain_deb_installer_smoke PROPERTIES
                RUN_SERIAL TRUE
                TIMEOUT 900
                SKIP_REGULAR_EXPRESSION "Skipping Linux installer smoke")
        zanna_add_test_labels(linux_toolchain_deb_installer_smoke installer tools requires_linux requires_linux_deb requires_privileged_install slow)

        zanna_add_ctest(linux_toolchain_rpm_installer_smoke
                ${CMAKE_COMMAND}
                -DCMAKE_BIN=${CMAKE_COMMAND}
                -DZANNA_BIN=$<TARGET_FILE:zanna>
                -DZANNA_BUILD_DIR=${CMAKE_BINARY_DIR}
                -DZANNA_CONFIG=$<CONFIG>
                -P ${_ZANNA_TOOLS_DIR}/LinuxToolchainRpmInstallerSmoke.cmake)
        set_tests_properties(linux_toolchain_rpm_installer_smoke PROPERTIES
                RUN_SERIAL TRUE
                TIMEOUT 900
                SKIP_REGULAR_EXPRESSION "Skipping Linux installer smoke")
        zanna_add_test_labels(linux_toolchain_rpm_installer_smoke installer tools requires_linux requires_linux_rpm requires_privileged_install slow)

        zanna_add_ctest(linux_toolchain_bundle_smoke
                ${CMAKE_COMMAND}
                -DCMAKE_BIN=${CMAKE_COMMAND}
                -DZANNA_BIN=$<TARGET_FILE:zanna>
                -DZANNA_BUILD_DIR=${CMAKE_BINARY_DIR}
                -DZANNA_CONFIG=$<CONFIG>
                -P ${_ZANNA_TOOLS_DIR}/LinuxToolchainBundleSmoke.cmake)
        set_tests_properties(linux_toolchain_bundle_smoke PROPERTIES
                RUN_SERIAL TRUE
                TIMEOUT 420
                SKIP_REGULAR_EXPRESSION "Skipping Linux self-extracting bundle smoke")
        zanna_add_test_labels(linux_toolchain_bundle_smoke installer tools requires_linux slow)
    endif ()

    if (WIN32)
        zanna_add_ctest(windows_update_manifest_authoring
                powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass
                -File ${_ZANNA_TOOLS_DIR}/WindowsUpdateManifestAuthoringTests.ps1
                -ManifestScript ${CMAKE_SOURCE_DIR}/scripts/new-windows-update-manifest.ps1
                -WorkDir ${CMAKE_CURRENT_BINARY_DIR}/windows_update_manifest_authoring)
        set_tests_properties(windows_update_manifest_authoring PROPERTIES
                RUN_SERIAL TRUE
                TIMEOUT 120)
        zanna_add_test_labels(windows_update_manifest_authoring installer tools requires_windows)

        zanna_add_ctest(windows_automation_script_contracts
                powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass
                -File ${_ZANNA_TOOLS_DIR}/WindowsAutomationScriptTests.ps1
                -SignScript ${CMAKE_SOURCE_DIR}/scripts/sign-windows-installer.ps1
                -DemoScript ${CMAKE_SOURCE_DIR}/scripts/build_demos_win.ps1
                -IdeScript ${CMAKE_SOURCE_DIR}/scripts/build_ide_win.ps1
                -InstallerScript ${CMAKE_SOURCE_DIR}/scripts/build_installer.ps1
                -BuildScript ${CMAKE_SOURCE_DIR}/scripts/build_zanna_win.ps1
                -ValidatorScript ${CMAKE_SOURCE_DIR}/scripts/validate-windows-toolchain-installer.ps1
                -WorkDir ${CMAKE_CURRENT_BINARY_DIR}/windows_automation_script_contracts)
        set_tests_properties(windows_automation_script_contracts PROPERTIES
                RUN_SERIAL TRUE
                TIMEOUT 120)
        zanna_add_test_labels(windows_automation_script_contracts installer tools requires_windows)

        zanna_add_ctest(windows_installer_user_smoke
                ${CMAKE_COMMAND}
                -DZANNA_BIN=$<TARGET_FILE:zanna>
                -DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/windows_installer_user_smoke
                -P ${_ZANNA_TOOLS_DIR}/WindowsUserInstallerSmoke.cmake)
        set_tests_properties(windows_installer_user_smoke PROPERTIES
                RUN_SERIAL TRUE
                TIMEOUT 300
                SKIP_REGULAR_EXPRESSION "Skipping .*Windows user installer smoke")
        zanna_add_test_labels(windows_installer_user_smoke installer tools requires_windows slow)

        zanna_add_ctest(windows_installer_crackman_smoke
                ${CMAKE_COMMAND}
                -DZANNA_BIN=$<TARGET_FILE:zanna>
                -DZANNA_REPO_ROOT=${CMAKE_SOURCE_DIR}
                -DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/windows_installer_crackman_smoke
                -P ${_ZANNA_TOOLS_DIR}/WindowsCrackmanInstallerSmoke.cmake)
        set_tests_properties(windows_installer_crackman_smoke PROPERTIES
                RUN_SERIAL TRUE
                TIMEOUT 540
                SKIP_REGULAR_EXPRESSION "Skipping .*Crackman Windows installer smoke")
        zanna_add_test_labels(windows_installer_crackman_smoke installer tools zia requires_windows slow)

        zanna_add_ctest(windows_installer_elevated_smoke
                ${CMAKE_COMMAND}
                -DZANNA_BIN=$<TARGET_FILE:zanna>
                -DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/windows_installer_elevated_smoke
                -P ${_ZANNA_TOOLS_DIR}/WindowsInstallerSmoke.cmake)
        set_tests_properties(windows_installer_elevated_smoke PROPERTIES
                RUN_SERIAL TRUE
                TIMEOUT 240
                SKIP_REGULAR_EXPRESSION "Skipping .*Windows installer smoke")
        zanna_add_test_labels(windows_installer_elevated_smoke installer tools requires_windows requires_privileged_install slow)
    endif ()

    zanna_add_test(test_cli_run_missing_main
            ${ZANNA_TESTS_DIR}/unit/test_cli_run_missing_main.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/cli.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/cmd_run_il.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/break_spec.cpp)
    target_include_directories(test_cli_run_missing_main PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_cli_run_missing_main PRIVATE ${ZANNA_TOOLS_LIBS})
    zanna_add_ctest(test_cli_run_missing_main test_cli_run_missing_main)

    zanna_add_test(test_cli_run_invalid_break_line
            ${ZANNA_TESTS_DIR}/unit/test_cli_run_invalid_break_line.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/cli.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/cmd_run_il.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/break_spec.cpp)
    target_include_directories(test_cli_run_invalid_break_line PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_cli_run_invalid_break_line PRIVATE ${ZANNA_TOOLS_LIBS})
    zanna_add_ctest(test_cli_run_invalid_break_line test_cli_run_invalid_break_line)

    zanna_add_test(test_cli_run_break_src_whitespace
            ${ZANNA_TESTS_DIR}/unit/test_cli_run_break_src_whitespace.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/cli.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/cmd_run_il.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/break_spec.cpp)
    target_include_directories(test_cli_run_break_src_whitespace PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_cli_run_break_src_whitespace PRIVATE ${ZANNA_TOOLS_LIBS})
    zanna_add_ctest(test_cli_run_break_src_whitespace test_cli_run_break_src_whitespace)

    zanna_add_test(test_cli_run_break_src_line_padding
            ${ZANNA_TESTS_DIR}/unit/test_cli_run_break_src_line_padding.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/cli.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/cmd_run_il.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/break_spec.cpp)
    target_include_directories(test_cli_run_break_src_line_padding PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_cli_run_break_src_line_padding PRIVATE ${ZANNA_TOOLS_LIBS})
    zanna_add_ctest(test_cli_run_break_src_line_padding test_cli_run_break_src_line_padding)

    zanna_add_test(test_cli_run_break_src_plain
            ${ZANNA_TESTS_DIR}/unit/test_cli_run_break_src_plain.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/cli.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/cmd_run_il.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/break_spec.cpp)
    target_include_directories(test_cli_run_break_src_plain PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_cli_run_break_src_plain PRIVATE ${ZANNA_TOOLS_LIBS})
    zanna_add_ctest(test_cli_run_break_src_plain test_cli_run_break_src_plain)

    zanna_add_test(test_cli_front_basic_verify_diag
            ${ZANNA_TESTS_DIR}/unit/test_cli_front_basic_verify_diag.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/cli.cpp)
    target_include_directories(test_cli_front_basic_verify_diag PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_cli_front_basic_verify_diag PRIVATE ${ZANNA_TOOLS_LIBS})
    zanna_add_ctest(test_cli_front_basic_verify_diag test_cli_front_basic_verify_diag)

    zanna_add_test(test_cli_front_basic_reject_break
            ${_ZANNA_TOOLS_DIR}/FrontBasicRejectBreakFlagTests.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/cli.cpp)
    target_include_directories(test_cli_front_basic_reject_break PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_cli_front_basic_reject_break PRIVATE ${ZANNA_TOOLS_LIBS})
    zanna_add_ctest(test_cli_front_basic_reject_break test_cli_front_basic_reject_break)

    zanna_add_test(test_cli_run_invalid_max_steps
            ${ZANNA_TESTS_DIR}/unit/test_cli_run_invalid_max_steps.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/cli.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/cmd_run_il.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/break_spec.cpp)
    target_include_directories(test_cli_run_invalid_max_steps PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_cli_run_invalid_max_steps PRIVATE ${ZANNA_TOOLS_LIBS})
    zanna_add_ctest(test_cli_run_invalid_max_steps test_cli_run_invalid_max_steps)

    zanna_add_test(test_cli_run_return_overflow
            ${ZANNA_TESTS_DIR}/unit/test_cli_run_return_overflow.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/cli.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/cmd_run_il.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/break_spec.cpp)
    target_include_directories(test_cli_run_return_overflow PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_cli_run_return_overflow PRIVATE ${ZANNA_TOOLS_LIBS})
    zanna_add_ctest(test_cli_run_return_overflow test_cli_run_return_overflow)

    zanna_add_test(test_cli_run_bounds_checks_unsupported
            ${ZANNA_TESTS_DIR}/unit/test_cli_run_bounds_checks_unsupported.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/cli.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/cmd_run_il.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/break_spec.cpp)
    target_include_directories(test_cli_run_bounds_checks_unsupported PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_cli_run_bounds_checks_unsupported PRIVATE ${ZANNA_TOOLS_LIBS})
    zanna_add_ctest(test_cli_run_bounds_checks_unsupported test_cli_run_bounds_checks_unsupported)

    zanna_add_test(test_cli_run_source_manager_overflow
            ${ZANNA_TESTS_DIR}/unit/test_cli_run_source_manager_overflow.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/cli.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/cmd_run_il.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/break_spec.cpp)
    target_include_directories(test_cli_run_source_manager_overflow PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_cli_run_source_manager_overflow PRIVATE ${ZANNA_TOOLS_LIBS})
    zanna_add_ctest(test_cli_run_source_manager_overflow test_cli_run_source_manager_overflow)

    zanna_add_test(test_cli_il_verify_source_manager_overflow
            ${ZANNA_TESTS_DIR}/unit/test_cli_il_verify_source_manager_overflow.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/il-verify/il-verify.cpp)
    target_include_directories(test_cli_il_verify_source_manager_overflow PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(test_cli_il_verify_source_manager_overflow PRIVATE ZANNA_IL_VERIFY_SKIP_MAIN)
    target_link_libraries(test_cli_il_verify_source_manager_overflow PRIVATE ${ZANNA_TOOLS_LIBS})
    zanna_add_ctest(test_cli_il_verify_source_manager_overflow test_cli_il_verify_source_manager_overflow)

    zanna_add_test(test_tools_il_verify_overflow_diag_once
            ${_ZANNA_TOOLS_DIR}/IlVerifyOverflowDiagOnceTests.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/il-verify/il-verify.cpp)
    target_include_directories(test_tools_il_verify_overflow_diag_once PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(test_tools_il_verify_overflow_diag_once PRIVATE ZANNA_IL_VERIFY_SKIP_MAIN)
    target_link_libraries(test_tools_il_verify_overflow_diag_once PRIVATE ${ZANNA_TOOLS_LIBS})
    zanna_add_ctest(test_tools_il_verify_overflow_diag_once test_tools_il_verify_overflow_diag_once)

    zanna_add_test(test_tools_il_verify_load_errors
            ${_ZANNA_TOOLS_DIR}/IlVerifyLoadErrorsTests.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/il-verify/il-verify.cpp)
    target_include_directories(test_tools_il_verify_load_errors PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(test_tools_il_verify_load_errors PRIVATE ZANNA_IL_VERIFY_SKIP_MAIN)
    target_link_libraries(test_tools_il_verify_load_errors PRIVATE ${ZANNA_TOOLS_LIBS})
    zanna_add_ctest(test_tools_il_verify_load_errors test_tools_il_verify_load_errors)

    zanna_add_test(test_cli_il_opt_passes
            ${ZANNA_TESTS_DIR}/unit/test_il_opt_passes.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/cli.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/cmd_il_opt.cpp)
    target_include_directories(test_cli_il_opt_passes PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_cli_il_opt_passes PRIVATE ${ZANNA_TOOLS_LIBS} il_transform)
    zanna_add_ctest(test_cli_il_opt_passes test_cli_il_opt_passes)

    zanna_add_test(test_cli_il_opt_pipeline
            ${ZANNA_TESTS_DIR}/unit/test_il_opt_pipeline.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/cli.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/cmd_il_opt.cpp)
    target_include_directories(test_cli_il_opt_pipeline PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_cli_il_opt_pipeline PRIVATE ${ZANNA_TOOLS_LIBS} il_transform)
    zanna_add_ctest(test_cli_il_opt_pipeline test_cli_il_opt_pipeline)

    # DCE pure call elimination test
    zanna_add_test(test_il_dce_pure_call
            ${ZANNA_TESTS_DIR}/unit/test_il_dce_pure_call.cpp)
    target_include_directories(test_il_dce_pure_call PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_il_dce_pure_call PRIVATE il_transform zanna_il_core)
    zanna_add_ctest(test_il_dce_pure_call test_il_dce_pure_call)

    # Benchmark output format test
    zanna_add_test(test_tools_bench_output_format
            ${_ZANNA_TOOLS_DIR}/BenchOutputFormatTests.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/cli.cpp
            ${CMAKE_SOURCE_DIR}/src/tools/zanna/cmd_bench.cpp)
    target_include_directories(test_tools_bench_output_format PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_tools_bench_output_format PRIVATE ${ZANNA_TOOLS_LIBS})
    zanna_add_ctest(test_tools_bench_output_format test_tools_bench_output_format)

    zanna_add_ctest(installer_tarball_smoke
            ${CMAKE_COMMAND}
            -DZANNA_BIN=$<TARGET_FILE:zanna>
            -DZANNA_BUILD_DIR=${CMAKE_BINARY_DIR}
            -DZANNA_CONFIG=$<CONFIG>
            -P ${_ZANNA_TOOLS_DIR}/InstallPackageTarballSmoke.cmake)
    set_tests_properties(installer_tarball_smoke PROPERTIES TIMEOUT 60)
    zanna_add_test_labels(installer_tarball_smoke installer tools slow)

    set(_ZANNA_INSTALLED_NATIVE_SMOKE 0)
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64" AND ZANNA_BUILD_NATIVE_LINK_X86_64)
        set(_ZANNA_INSTALLED_NATIVE_SMOKE 1)
    elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64" AND ZANNA_BUILD_NATIVE_LINK_AARCH64)
        set(_ZANNA_INSTALLED_NATIVE_SMOKE 1)
    endif ()

    zanna_add_ctest(installer_installed_config_smoke
            ${CMAKE_COMMAND}
            -DCMAKE_BIN=${CMAKE_COMMAND}
            -DZANNA_BIN=$<TARGET_FILE:zanna>
            -DZANNA_BUILD_DIR=${CMAKE_BINARY_DIR}
            -DZANNA_CONFIG=$<CONFIG>
            -DZANNA_RUN_NATIVE_CODEGEN=${_ZANNA_INSTALLED_NATIVE_SMOKE}
            -P ${_ZANNA_TOOLS_DIR}/InstalledZannaConfigSmoke.cmake)
    zanna_add_test_labels(installer_installed_config_smoke installer tools slow)

    if (WIN32 AND ZANNA_ENABLE_WINDOWS_INSTALLER_E2E)
        zanna_add_ctest(installer_windows_toolchain_e2e
                ${CMAKE_COMMAND}
                -DCMAKE_BIN=${CMAKE_COMMAND}
                -DZANNA_BIN=$<TARGET_FILE:zanna>
                -DZANNA_BUILD_DIR=${CMAKE_BINARY_DIR}
                -DZANNA_CONFIG=$<CONFIG>
                -DZANNA_MSVC_REDIST_DIR=${MSVC_REDIST_DIR}
                -DZANNA_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}
                -DZANNA_INSTALLER_TEST_HOOKS=${ZANNA_INSTALLER_ENABLE_TEST_HOOKS}
                -P ${_ZANNA_TOOLS_DIR}/WindowsToolchainInstallerE2E.cmake)
        set_tests_properties(installer_windows_toolchain_e2e PROPERTIES RUN_SERIAL TRUE TIMEOUT 1800)
        zanna_add_test_labels(installer_windows_toolchain_e2e installer tools slow e2e)
    endif ()
endfunction()
