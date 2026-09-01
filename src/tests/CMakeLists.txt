#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: src/tests/CMakeLists.txt
# Purpose: Configure the complete Zanna CTest graph and platform-specific fixtures.
# Key invariants:
#   - Tests declare the labels, resource locks, timeouts, and host gates they require.
#   - Platform-only tests never leak unavailable binaries into another host's graph.
# Ownership/Lifetime: CMake owns all generated test targets and fixture paths.
# Links: docs/internals/testing.md, scripts/build_zanna_win.ps1
#
#===----------------------------------------------------------------------===#
#
# ============================================================================
# Zanna Test Suite — Quick Reference
# ============================================================================
# Labels (use with ctest -L <label>):
#   basic       — BASIC frontend tests (lexer, parser, sema, lowerer)
#   bytecode    — Bytecode VM tests and bytecode/IL VM parity
#   il          — IL core tests (parsing, serialization, verification, analysis)
#   vm          — VM runtime tests (opcodes, traps, debugging, concurrency)
#   runtime     — C runtime library tests (strings, collections, I/O, etc.)
#   codegen     — Code generation tests (x86_64, AArch64, linker, binary encoding)
#   oop         — Object-oriented programming tests
#   golden      — Golden file regression tests (diagnostic messages, IL output)
#   e2e         — End-to-end pipeline tests (source -> IL -> VM/native -> compare)
#   examples    — Example/demo manifest audit and build smoke
#   fuzz        — Fuzz corpus replay / fuzz harness checks
#   ilopt       — IL optimizer pass tests
#   conformance — Arithmetic semantics cross-layer equivalence
#   zia         — Zia language frontend tests
#   namespace   — Namespace/module system tests
#   tools       — CLI tool tests, language server tests
#   audit       — Local source-health and structural drift audits
#   smoke       — Quick sanity tests
#   unit        — Focused unit tests for support libraries
#   perf        — Performance benchmarks (excluded from default runs)
#   slow        — Long-running tests (excluded from default runs)
#   tui         — Terminal UI / REPL tests
#   arm64       — ARM64-only native/backend coverage
#   native_link — Native link/assemble execution paths
#   comprehensive — Large end-to-end feature sweeps
#   syntax_only — Syntax-check-only native codegen coverage
#   assemble_link — Native assemble+link coverage
#   native_run  — Native compile+run coverage
#   requires_display — Needs a display server / real windowing
#   requires_local_bind — Needs local loopback bind/listen support
#   requires_ipv6 — Needs IPv6 loopback support
#   requires_audio_disabled — Only meaningful in no-audio builds
#   requires_graphics_disabled — Only meaningful in no-graphics builds
#   requires_posix_shell — Uses POSIX shell/scripts
#   windows_broken — Known broken on Windows until fixed
#
# Common commands:
#   ctest --test-dir build                     # Run all tests
#   ctest --test-dir build -L codegen          # Run codegen tests only
#   ctest --test-dir build -R test_zia_lexer   # Run specific test
#   ctest --test-dir build --print-labels      # List all labels
#   ctest --test-dir build -j$(nproc)          # Parallel execution
# ============================================================================

cmake_minimum_required(VERSION 3.20)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/TestHelpers.cmake)

## Docs lint disabled (no external scripting dependencies)

set(ZANNA_TESTS_DIR ${CMAKE_CURRENT_LIST_DIR})

if (WIN32)
    if (DEFINED ENV{SystemDrive})
        file(MAKE_DIRECTORY "$ENV{SystemDrive}/tmp")
    else ()
        file(MAKE_DIRECTORY "C:/tmp")
    endif ()
endif ()

file(GLOB ZANNA_TEST_SCRIPT_FILES
        LIST_DIRECTORIES FALSE
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/*.sh")
foreach (_zanna_test_script IN LISTS ZANNA_TEST_SCRIPT_FILES)
    file(
            CHMOD "${_zanna_test_script}"
            PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
            GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE)
endforeach ()

find_package(Threads REQUIRED)
find_program(ZANNA_BASH_EXECUTABLE NAMES bash)
set(ZANNA_BASH_COMMAND)
if (ZANNA_BASH_EXECUTABLE)
    set(ZANNA_BASH_COMMAND "${ZANNA_BASH_EXECUTABLE}")
    if (WIN32)
        # Git Bash must run as a login shell so /usr/bin tools and the inherited
        # Windows toolchain PATH are initialized consistently for CTest.
        list(APPEND ZANNA_BASH_COMMAND "--login")
    endif ()
endif ()

if (WIN32)
    add_test(NAME windows_installer_host_launch
            COMMAND $<TARGET_FILE:zanna_installer_host> /selftest /quiet)
    add_test(NAME windows_installer_host_cli_contracts
            COMMAND ${CMAKE_COMMAND}
                    -DHOST=$<TARGET_FILE:zanna_installer_host>
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/WindowsInstallerHostCliContracts.cmake)
    add_test(NAME windows_utf8_tool_command_line
            COMMAND ${CMAKE_COMMAND}
                    -DZANNA=$<TARGET_FILE:zanna>
                    -DZIA=$<TARGET_FILE:zia>
                    -DZBASIC=$<TARGET_FILE:zbasic>
                    -DRTGEN=$<TARGET_FILE:rtgen>
                    -DRUNTIME_DEF=${PROJECT_SOURCE_DIR}/src/il/runtime/runtime.def
                    -DWORK_ROOT=${CMAKE_CURRENT_BINARY_DIR}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/WindowsUtf8ToolCommandLine.cmake)
    add_test(NAME windows_installer_cleanup_launch
            COMMAND $<TARGET_FILE:zanna_installer_cleanup> /selftest)
    set_tests_properties(
            windows_installer_host_launch
            windows_installer_host_cli_contracts
            windows_utf8_tool_command_line
            windows_installer_cleanup_launch
            PROPERTIES
            LABELS "installer;tools;unit"
            TIMEOUT 15)
endif ()

if (ZANNA_BASH_EXECUTABLE)
    set(_zanna_source_health_audit_timeout 60)
    if (WIN32)
        set(_zanna_source_health_audit_timeout 300)
    endif ()

    add_test(NAME check_vm_semantics_duplication
            COMMAND ${ZANNA_BASH_COMMAND}
            scripts/check_vm_semantics_duplication.sh
            .)
    set_tests_properties(check_vm_semantics_duplication PROPERTIES
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            LABELS "conformance;vm;bytecode;requires_posix_shell")

    add_test(NAME zia_runtime_name_literal_lint
            COMMAND ${ZANNA_BASH_COMMAND}
            scripts/lint_zia_runtime_names.sh
            .)
    set_tests_properties(zia_runtime_name_literal_lint PROPERTIES
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            LABELS "tools;zia;requires_posix_shell")

    add_test(NAME benchmark_compare_self_test
            COMMAND ${ZANNA_BASH_COMMAND}
            scripts/benchmark_compare.sh
            --self-test)
    set_tests_properties(benchmark_compare_self_test PROPERTIES
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            LABELS "perf;tools;requires_posix_shell")

    # The full sanitizer driver configures Clang's POSIX runtime lanes. A Git
    # Bash executable is enough for the other shell checks on Windows, but it
    # does not make the Windows compiler-rt toolchain compatible with this
    # driver (notably the UBSan runtime/link model).
    if (NOT WIN32)
        add_test(NAME sanitizer_script_self_test
                COMMAND ${ZANNA_BASH_COMMAND}
                scripts/ci_full_sanitizer.sh
                --self-test)
        set_tests_properties(sanitizer_script_self_test PROPERTIES
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                SKIP_REGULAR_EXPRESSION "SKIP:"
                LABELS "tools;requires_posix_shell")
    endif ()

    if (WIN32 AND CLANG_EXECUTABLE)
        get_filename_component(_zanna_coverage_llvm_bin "${CLANG_EXECUTABLE}" DIRECTORY)
        string(REPLACE ";" "\\;" _zanna_coverage_host_path "$ENV{PATH}")
        add_test(NAME coverage_script_self_test
                COMMAND "${CMAKE_COMMAND}" -E env
                "PATH=${_zanna_coverage_llvm_bin}\;${_zanna_coverage_host_path}"
                ${ZANNA_BASH_COMMAND}
                scripts/coverage.sh
                --self-test)
        unset(_zanna_coverage_host_path)
        unset(_zanna_coverage_llvm_bin)
    else ()
        add_test(NAME coverage_script_self_test
                COMMAND ${ZANNA_BASH_COMMAND}
                scripts/coverage.sh
                --self-test)
    endif ()
    set_tests_properties(coverage_script_self_test PROPERTIES
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            SKIP_REGULAR_EXPRESSION "SKIP:"
            LABELS "tools;requires_posix_shell")

    add_test(NAME example_smoke_manifest_audit
            COMMAND ${ZANNA_BASH_COMMAND}
            scripts/example_smoke.sh
            --audit)
    set_tests_properties(example_smoke_manifest_audit PROPERTIES
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            LABELS "examples;requires_posix_shell")

    add_test(NAME demo_project_manifest_audit
            COMMAND ${ZANNA_BASH_COMMAND}
            scripts/check_demo_projects.sh)
    set_tests_properties(demo_project_manifest_audit PROPERTIES
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            LABELS "build;examples;requires_posix_shell")

    add_test(NAME example_smoke_fast
            COMMAND ${ZANNA_BASH_COMMAND}
            scripts/example_smoke.sh
            --fast
            --zanna $<TARGET_FILE:zanna>)
    set_tests_properties(example_smoke_fast PROPERTIES
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            LABELS "examples;smoke;requires_posix_shell"
            TIMEOUT 120)

    # Bridge to the externalized demo suite (the zannademos repo cloned at
    # <root>/zannademos). Absence of the clone is a visible Skipped result,
    # never a silent gap; see docs/adr/0241.
    add_test(NAME zannademos_smoke
            COMMAND ${ZANNA_BASH_COMMAND}
            scripts/zannademos_bridge.sh
            --fast
            --zanna $<TARGET_FILE:zanna>)
    set_tests_properties(zannademos_smoke PROPERTIES
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            LABELS "demos;requires_posix_shell"
            SKIP_REGULAR_EXPRESSION "Skipping zannademos"
            TIMEOUT 900)

    add_test(NAME zannademos_full
            COMMAND ${ZANNA_BASH_COMMAND}
            scripts/zannademos_bridge.sh
            --full
            --zanna $<TARGET_FILE:zanna>)
    set_tests_properties(zannademos_full PROPERTIES
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            LABELS "demos;slow;requires_posix_shell"
            SKIP_REGULAR_EXPRESSION "Skipping zannademos"
            RUN_SERIAL TRUE
            TIMEOUT 5400)

    add_test(NAME fuzz_smoke_self_test
            COMMAND ${ZANNA_BASH_COMMAND}
            scripts/fuzz_smoke.sh
            --self-test)
    set_tests_properties(fuzz_smoke_self_test PROPERTIES
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            LABELS "fuzz;tools;requires_posix_shell")

    add_test(NAME source_health_audit
            COMMAND ${ZANNA_BASH_COMMAND}
            scripts/source_health_audit.sh
            --check)
    set_tests_properties(source_health_audit PROPERTIES
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            LABELS "tools;audit;requires_posix_shell;slow"
            TIMEOUT ${_zanna_source_health_audit_timeout})
endif ()

add_library(zanna_testing INTERFACE)
target_link_libraries(zanna_testing INTERFACE zanna_common_opts zanna::il_full)

add_library(zanna_test_common STATIC
        ${CMAKE_CURRENT_LIST_DIR}/common/CodegenFixture.cpp
        ${CMAKE_CURRENT_LIST_DIR}/common/ILGenerator.cpp
        ${CMAKE_CURRENT_LIST_DIR}/common/ProcessIsolation.cpp
        ${CMAKE_CURRENT_LIST_DIR}/common/TestIRBuilder.cpp
        ${CMAKE_CURRENT_LIST_DIR}/common/VmFixture.cpp)
# Limit include paths to avoid clashing with the C++ <version> header on
# case-insensitive filesystems (root contains a VERSION file).
# Expose only tests dir, public headers, generated headers, and src as needed.
target_include_directories(zanna_test_common PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}/generated
        ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(zanna_test_common PUBLIC zanna_testing il_vm il_build)

## docs_comment_check removed to avoid Python dependency

add_subdirectory(unit)
add_subdirectory(bytecode)
add_subdirectory(il)
add_subdirectory(vm)
add_subdirectory(perf)
add_subdirectory(tools)
add_subdirectory(basic)
add_subdirectory(zia)
add_subdirectory(golden)
add_subdirectory(e2e)

# ── Zia Language Server tests ──────────────────────────────────────────────
set(_ZIA_SERVER_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/zia-server)

zanna_add_test(test_zia_server_json ${_ZIA_SERVER_TEST_DIR}/test_json.cpp
        LIBS zia_server_lib)

zanna_add_test(test_zia_server_transport ${_ZIA_SERVER_TEST_DIR}/test_transport.cpp
        LIBS zia_server_lib)

zanna_add_test(test_zia_server_bridge ${_ZIA_SERVER_TEST_DIR}/test_compiler_bridge.cpp
        LIBS zia_server_lib)

zanna_add_test(test_zia_server_mcp ${_ZIA_SERVER_TEST_DIR}/test_mcp_handler.cpp
        LIBS zia_server_lib)

zanna_add_test(test_zia_server_lsp ${_ZIA_SERVER_TEST_DIR}/test_lsp_handler.cpp
        LIBS zia_server_lib)

# ── BASIC Language Server tests ───────────────────────────────────────────
set(_ZBASIC_SERVER_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/zbasic-server)

zanna_add_test(test_zbasic_server_bridge ${_ZBASIC_SERVER_TEST_DIR}/test_basic_compiler_bridge.cpp
        LIBS zbasic_server_lib)

zanna_add_test(test_zbasic_server_mcp ${_ZBASIC_SERVER_TEST_DIR}/test_basic_server_mcp.cpp
        LIBS zbasic_server_lib)

zanna_add_test(test_zbasic_server_lsp ${_ZBASIC_SERVER_TEST_DIR}/test_basic_server_lsp.cpp
        LIBS zbasic_server_lib)

# ── BASIC Analysis & Completion tests ─────────────────────────────────────
zanna_add_test(test_basic_analysis
        ${CMAKE_CURRENT_SOURCE_DIR}/basic/test_basic_analysis.cpp
        LIBS fe_basic zanna::il_full)

zanna_add_test(test_basic_completion
        ${CMAKE_CURRENT_SOURCE_DIR}/basic/test_basic_completion.cpp
        LIBS fe_basic zanna::il_full)

# ── Fuzz harnesses (opt-in, not registered as ctest targets) ────────────────
if (ZANNA_ENABLE_FUZZ)
    add_subdirectory(fuzz)
endif ()
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/codegen/x86_64)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/codegen/objfile)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/codegen/binenc)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/codegen/linker)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/codegen/common)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/codegen/aarch64)

# ── helpers for AArch64 and cross-platform codegen tests ────────────────────
# Defined at file scope so they are available both inside and outside the
# ARM64-gated block below.
macro(_aarch64_add_test _name _src)
    add_executable(${_name} ${_src})
    if (WIN32)
        target_sources(${_name} PRIVATE ${ZANNA_TESTS_DIR}/common/WinDialogSuppress.c)
    endif ()
    target_link_libraries(${_name} PRIVATE zanna_testing ${ARGN})
    zanna_add_ctest(${_name} $<TARGET_FILE:${_name}>)
endmacro()

macro(_aarch64_add_cmd_test _name _src)
    _aarch64_add_test(${_name} ${_src} ${ARGN})
    set_tests_properties(${_name} PROPERTIES RESOURCE_LOCK aarch64_codegen_artifacts)
endmacro()

macro(_aarch64_add_test_common _name _src)
    add_executable(${_name} ${_src})
    if (WIN32)
        target_sources(${_name} PRIVATE ${ZANNA_TESTS_DIR}/common/WinDialogSuppress.c)
    endif ()
    target_link_libraries(${_name} PRIVATE zanna_test_common ${ARGN})
    zanna_add_ctest(${_name} $<TARGET_FILE:${_name}>)
endmacro()

# ── Pure AArch64 backend tests: build on every host; they do not execute
# generated ARM64 code or require a native ARM64 assembler.
set(_AARCH64_CODEGEN_ONLY_TESTS
        test_target_aarch64
        test_emit_aarch64_minimal
        test_emit_aarch64_mir_minimal
        test_emit_aarch64_frameplan
        test_emit_aarch64_mir_bitwise
        test_emit_aarch64_mir_branches
        test_emit_aarch64_mir_frameplan
        test_codegen_arm64_peephole
        test_codegen_arm64_div_strength
        test_regalloc_aarch64_linear
        test_aarch64_rodata_pool
        test_codegen_review_fixes
        test_aarch64_frame_spill_reuse
        test_aarch64_logical_imm
        test_codegen_arm64_peephole_subpasses
)
foreach (_test IN LISTS _AARCH64_CODEGEN_ONLY_TESTS)
    _aarch64_add_test(${_test}
            ${CMAKE_CURRENT_SOURCE_DIR}/unit/codegen/${_test}.cpp
            "il_codegen_aarch64")
endforeach ()

# AArch64 modular pipeline and ABI tests are also pure codegen tests.
_aarch64_add_test_common(test_aarch64_pass_manager
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/codegen/test_aarch64_pass_manager.cpp
        "il_codegen_aarch64")

_aarch64_add_test_common(test_aarch64_scheduler
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/codegen/test_aarch64_scheduler.cpp
        "il_codegen_aarch64")

_aarch64_add_test_common(test_aarch64_linux_abi
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/codegen/test_aarch64_linux_abi.cpp
        "il_codegen_aarch64")

_aarch64_add_test_common(test_aarch64_block_layout
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/codegen/test_aarch64_block_layout.cpp
        "il_codegen_aarch64")

# ARM64/AArch64 command/native tests — gated on ARM64 processor.
# Windows ARM64 builds the backend, but the current native/link test path still
# assumes Unix/Darwin assembler and process semantics.
if (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64" AND NOT WIN32)

    # ── Group 2: il_codegen_aarch64 + zanna_cmd_arm64 ──────────────────
    set(_AARCH64_CMD_TESTS
            test_codegen_arm64_cli
            test_codegen_arm64_add_params
            test_codegen_arm64_ret_param
            test_codegen_arm64_add_imm
            test_codegen_arm64_bitwise
            test_codegen_arm64_params_wide
            test_codegen_arm64_ovf
            test_codegen_arm64_shift_imm
            test_codegen_arm64_stack_locals
            test_codegen_arm64_stack_locals_cli
            test_codegen_arm64_gep_load_store
            test_codegen_arm64_array_access
            test_codegen_arm64_const_str_addr
            test_codegen_arm64_mir_dump
            test_codegen_arm64_ra_many_temps
            test_codegen_arm64_icmp
            test_codegen_arm64_large_imm
            test_codegen_arm64_icmp_imm
            test_codegen_arm64_switch
            test_codegen_arm64_trap_eh
            test_codegen_arm64_run_native
            test_codegen_arm64_bugfix
            test_codegen_arm64_cbr
            test_codegen_arm64_call
            test_codegen_arm64_call_args
            test_codegen_arm64_call_mid_block
            test_codegen_arm64_cf_if_else_phi
            test_codegen_arm64_select
            test_codegen_arm64_cf_loop_phi
            test_codegen_arm64_call_temp_args
            test_codegen_arm64_print_str
            test_codegen_arm64_dead_strip
            test_codegen_arm64_modvar_addr
            test_codegen_arm64_emit_and_link
            test_codegen_arm64_arr_obj_len
            test_codegen_arm64_obj_field_gep
            test_codegen_arm64_arr_obj_put_get
            test_codegen_arm64_call_stack_args
            test_codegen_arm64_call_stack_args_multi
            test_codegen_arm64_callee_stack_params
            test_codegen_arm64_call_convention
            test_codegen_arm64_fp
            test_codegen_arm64_indirect_call
            test_codegen_arm64_idxchk
            test_codegen_arm64_division
            test_codegen_arm64_exception_handling
            test_codegen_arm64_string_store_refcount
            test_codegen_arm64_sub_mul
            test_codegen_arm64_shift_reg
            test_codegen_arm64_fp_cmp_all
            test_codegen_arm64_gaddr_constnull
            test_codegen_arm64_spill_fpr
            test_codegen_arm64_cross_block_phi_spill
            test_codegen_arm64_unsigned_cmp
            test_codegen_arm64_peephole_comprehensive
            test_codegen_arm64_abi_mixed_args
            test_codegen_arm64_rodata_literals
            test_codegen_arm64_callee_saved
            test_codegen_arm64_mixed_call_f64
            test_aarch64_phi_coalescer
            test_aarch64_global_liveness
            test_codegen_arm64_il_opt
            test_codegen_arm64_leaf_func
            test_codegen_arm64_native_asm
            test_codegen_arm64_benchmark_regressions
            test_codegen_arm64_tbz_fusion
    )
    foreach (_test IN LISTS _AARCH64_CMD_TESTS)
        _aarch64_add_cmd_test(${_test}
                ${CMAKE_CURRENT_SOURCE_DIR}/unit/codegen/${_test}.cpp
                "il_codegen_aarch64;zanna_cmd_arm64")
    endforeach ()

    _aarch64_add_cmd_test(test_codegen_arm64_casts
            ${CMAKE_CURRENT_SOURCE_DIR}/unit/codegen/test_codegen_arm64_casts.cpp
            "il_codegen_aarch64;zanna_cmd_arm64")

    # CLI run-native should return user exit code
    # Note: run-native exit code coverage exists in other tests; keep CI stable.
    # _aarch64_add_test(test_codegen_arm64_run_ret42
    #     ${CMAKE_CURRENT_SOURCE_DIR}/unit/codegen/test_codegen_arm64_run_ret42.cpp
    #     "il_codegen_aarch64;zanna_cmd_arm64")

    # ── Group 3: special link patterns ──────────────────────────────────

    # Environment.IsNative should distinguish VM vs native paths
    _aarch64_add_test_common(test_codegen_env_is_native
            ${CMAKE_CURRENT_SOURCE_DIR}/unit/codegen/test_codegen_env_is_native.cpp
            "il_codegen_aarch64;zanna_cmd_arm64")
    set_tests_properties(test_codegen_env_is_native PROPERTIES RESOURCE_LOCK aarch64_codegen_artifacts)

    # Property-based differential testing: VM vs ARM64 native
    # This test must run alone (RUN_SERIAL) to avoid resource contention with other
    # codegen tests that use the same assembler/linker toolchain.
    _aarch64_add_test_common(test_diff_vm_native_property
            ${CMAKE_CURRENT_SOURCE_DIR}/unit/codegen/test_diff_vm_native_property.cpp
            "il_codegen_aarch64;zanna_cmd_arm64")
    set_tests_properties(test_diff_vm_native_property PROPERTIES
            RUN_SERIAL TRUE
            TIMEOUT 120
            LABELS "slow"
            RESOURCE_LOCK aarch64_codegen_artifacts)

    # ── ARM64 end-to-end demo tests ────────────────────────────────────
    set(_AARCH64_E2E_TESTS
            test_arm64_demos
            test_arm64_demos_run
    )
    foreach (_test IN LISTS _AARCH64_E2E_TESTS)
        _aarch64_add_cmd_test(${_test}
                ${CMAKE_CURRENT_SOURCE_DIR}/e2e/${_test}.cpp
                "zanna_common")
    endforeach ()

endif () # ARM64 processor tests

# ── Conformance test suite (arithmetic semantics, cross-layer equivalence) ──
# Must appear after _aarch64_add_test_common macro definition above.
add_subdirectory(conformance)

# ── Tests that exercise both AArch64 and x86-64 codegen but do not execute
# generated machine code — run on all platforms. ─────────────────────────────

# Cross-platform ABI invariants (CRIT-1 Win64 shadow space, CRIT-3 AArch64
# Windows ARM64 target, HIGH-4 LinkerSupport archive extension).
_aarch64_add_test_common(test_cross_platform_abi
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/codegen/test_cross_platform_abi.cpp
        "il_codegen_aarch64;zanna_codegen_x86_64")

# x86-64 runtime map — no ARM64 execution required.
_aarch64_add_test_common(test_emit_x86_runtime_map
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/codegen/test_emit_x86_runtime_map.cpp
        "zanna_codegen_x86_64")

# IL optimizer differential testing (VM baseline vs O0/O1/O2) — pure IL, no
# architecture dependency.
_aarch64_add_test_common(test_il_opt_equivalence
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_il_opt_equivalence.cpp
        "il_transform;zanna::il_verify")

_aarch64_add_test_common(test_il_backend_benchmark_regressions
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_il_backend_benchmark_regressions.cpp
        "il_transform;zanna::il_verify")

zanna_add_ctest(NoAssertFalseGuard
        ${CMAKE_COMMAND}
        -DZANNA_SOURCE_DIR=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_SOURCE_DIR}/src/tests/tools/NoAssertFalseTest.cmake)

zanna_add_ctest(test_tools_tls_signature_policy
        ${CMAKE_COMMAND}
        -DZANNA_SOURCE_DIR=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_SOURCE_DIR}/src/tests/tools/TlsSignaturePolicyTest.cmake)

zanna_add_ctest(test_tools_trap_kind_spec_consistency
        ${CMAKE_COMMAND}
        -DZANNA_SOURCE_DIR=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_SOURCE_DIR}/src/tests/tools/TrapKindSpecConsistencyTest.cmake)

zanna_add_ctest(smoke_term_basic
        $<TARGET_FILE:zanna> front basic -run ${CMAKE_CURRENT_SOURCE_DIR}/smoke/term_basic.bas)

# Shell script tests - skip on Windows
if (NOT WIN32)
    add_test(NAME basic_sum_no_linenos
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_sum_no_linenos.sh
            $<TARGET_FILE:zanna>)
    set_tests_properties(basic_sum_no_linenos
            PROPERTIES
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            LABELS smoke)
endif ()

# ── BASIC OOP unit tests ────────────────────────────────────────────────
zanna_add_test(test_basic_oop_parsing
        SRCS ${CMAKE_CURRENT_SOURCE_DIR}/unit/BasicOOP_Parsing.cpp
        TEST_NAME unit_basic_oop_parsing
        LIBS fe_basic)

zanna_add_test(test_basic_oop_lowering
        SRCS ${CMAKE_CURRENT_SOURCE_DIR}/unit/BasicOOP_Lowering.cpp
        TEST_NAME unit_basic_oop_lowering
        LIBS fe_basic)

## Removed: test_basic_oop_string_array_field, test_basic_oop_object_array_field
## Covered by test_basic_oop_numeric_array_field (same lowering pattern).

zanna_add_test(test_basic_global_string_array_sub_store
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_basic_global_string_array_sub_store.cpp
        LIBS fe_basic)

zanna_add_test(test_basic_select_case_object_assign
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_basic_select_case_object_assign.cpp
        LIBS fe_basic)

## Removed: test_basic_input_typing (covered by test_basic_lowerer_conversions)
## Removed: test_basic_nested_member_method_call (covered by BasicOOP_Lowering + TestInheritance)

zanna_add_test(test_basic_class_return
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_basic_class_return.cpp
        LIBS fe_basic)

zanna_add_test(test_basic_lower_builtin_dispatch
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_basic_lower_builtin_dispatch.cpp
        LIBS fe_basic)

# ── Runtime class catalog and binding tests ─────────────────────────────
## Removed: test_runtime_class_catalog (covered by TestCatalog.cpp)

zanna_add_test(test_runtime_property_binding
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime_classes/TestPropertyBinding.cpp
        LIBS fe_basic)

zanna_add_test(test_runtime_method_binding
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime_classes/TestMethodBinding.cpp
        LIBS fe_basic)

zanna_add_test(test_runtime_wishlist_bindings
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime_classes/TestRuntimeWishlistBindings.cpp
        LIBS fe_basic)

# TODO: Fix arity check for static methods (no receiver param). 1056 false
# positives because the test unconditionally adds +1 for receiver on all methods.
zanna_add_test(test_jpeg_decode
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime/TestJpegDecode.cpp
        LIBS zanna_runtime zanna_test_common)

zanna_add_test(test_ogg_vorbis
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime/TestOggVorbis.cpp
        LIBS zanna_runtime zanna_test_common)

zanna_add_test(test_gif_decode
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime/TestGifDecode.cpp
        LIBS zanna_runtime zanna_test_common)

zanna_add_test(test_mp3_decode
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime/TestMp3Decode.cpp
        LIBS zanna_runtime zanna_test_common)

# zannaaud only exists when an audio backend is available (ALSA/CoreAudio/WASAPI).
if (TARGET zannaaud)
    zanna_add_test(test_wav_stream
            ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime/TestWavStream.cpp
            LIBS zannaaud zanna_test_common
            INCLUDES ${CMAKE_SOURCE_DIR}/src/lib/audio/include ${CMAKE_SOURCE_DIR}/src/lib/audio/src)

    zanna_add_test(test_music_stream_thread
            ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime/TestMusicStreamThread.cpp
            LIBS zannaaud zanna_test_common
            INCLUDES ${CMAKE_SOURCE_DIR}/src/lib/audio/include ${CMAKE_SOURCE_DIR}/src/lib/audio/src)
endif ()

zanna_add_test(test_stl_load
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime/TestStlLoad.cpp
        LIBS zanna_runtime zanna_test_common
        INCLUDES ${CMAKE_SOURCE_DIR}/src/lib/graphics/include)

zanna_add_test(test_rt_behavior
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime/TestBehavior.cpp
        LIBS zanna_runtime zanna_test_common)

zanna_add_test(test_rt_typewriter
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime/TestTypewriter.cpp
        LIBS zanna_runtime zanna_test_common)

zanna_add_test(test_rt_lighting2d
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime/TestLighting2D.cpp
        LIBS zanna_runtime zanna_test_common)

zanna_add_test(test_rt_game_menu
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime/TestGameMenu.cpp
        LIBS zanna_runtime zanna_test_common)

zanna_add_test(test_rt_tilemap_anim
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime/TestTilemapAnim.cpp
        LIBS zanna_runtime zanna_test_common)

zanna_add_test(test_rt_raycast_2d
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime/TestRaycast2D.cpp
        LIBS zanna_runtime zanna_test_common)

zanna_add_test(test_rt_scene_manager
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime/TestSceneManager.cpp
        LIBS zanna_runtime zanna_test_common)

zanna_add_test(test_rt_entity
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime/TestEntity.cpp
        LIBS zanna_runtime zanna_test_common)

zanna_add_test(test_rt_animstate_named
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime/TestAnimStateNamed.cpp
        LIBS zanna_runtime zanna_test_common)

zanna_add_test(test_rt_animtimeline
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime/TestAnimTimeline.cpp
        LIBS zanna_runtime zanna_test_common)

zanna_add_test(test_rt_game_result_objects
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime/TestGameResultObjects.cpp
        LIBS zanna_runtime zanna_test_common)

zanna_add_test(test_rt_data_format_result_apis
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime/TestDataFormatResultApis.cpp
        LIBS zanna_runtime zanna_test_common)

zanna_add_test(test_rt_open_load_result_apis
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime/TestRuntimeOpenLoadResultApis.cpp
        LIBS zanna_runtime zanna_test_common)

zanna_add_test(test_rt_search_option_apis
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime/TestRuntimeSearchOptionApis.cpp
        LIBS zanna_runtime zanna_test_common)

zanna_add_test(test_rt_camera_enhance
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime/TestCameraEnhance.cpp
        LIBS zanna_runtime zanna_test_common)

zanna_add_test(test_rt_zpak_format
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime/TestZpakFormat.cpp
        LIBS zanna_runtime zanna_packaging zanna_test_common)

zanna_add_test(test_rt_asset_manager
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime/TestAssetManager.cpp
        LIBS zanna_runtime zanna_packaging zanna_test_common)

zanna_add_test(test_rt_path_exe_dir
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime/TestPathExeDir.cpp
        LIBS zanna_runtime zanna_test_common)

zanna_add_test(test_runtime_catalog_targets_resolve
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/runtime_classes/TestCatalogTargetsResolve.cpp
        LIBS fe_basic)

zanna_add_test(test_frontends_basic_runtime_property_diag
        ${CMAKE_CURRENT_SOURCE_DIR}/frontends/basic/RuntimePropertyDiagTests.cpp
        LIBS fe_basic)

zanna_add_test(test_basic_semantic_new_ctor
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_basic_semantic_new_ctor.cpp
        LIBS fe_basic)

zanna_add_test(test_basic_declare_foreign
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_basic_declare_foreign.cpp
        LIBS fe_basic)

zanna_add_test(test_run_process_quotes
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_run_process_quotes.cpp
        LIBS zanna_common)

zanna_add_ctest(smoke_basic_oop
        $<TARGET_FILE:zanna> front basic -run ${CMAKE_CURRENT_SOURCE_DIR}/smoke/basic_oop.bas)

zanna_add_ctest(smoke_basic_point_ctor
        $<TARGET_FILE:zanna> front basic -run ${CMAKE_CURRENT_SOURCE_DIR}/smoke/basic_point_ctor.bas)

zanna_add_unit_support_tests()
zanna_add_il_core_tests()
zanna_add_basic_early_tests()
zanna_add_il_utils_test()
zanna_add_tools_tests()
zanna_add_vm_debug_tests()
zanna_add_il_analysis_tests()
zanna_add_il_verify_examples()
zanna_add_e2e_vm_examples_smoke()
zanna_add_runtime_tests()
zanna_add_e2e_vm_suite()
zanna_add_il_invalid_tests()
zanna_add_basic_main_tests()
zanna_add_zia_tests()
zanna_add_vm_unit_tests()
zanna_add_il_liveness_tests()
zanna_add_vm_path_tests()
zanna_add_basic_ast_golden_tests()
zanna_add_basic_lex_golden_tests()
zanna_add_basic_to_il_golden_tests()
zanna_add_basic_strings_test()
zanna_add_basic_semantics_golden_tests()
zanna_add_basic_proc_tests()
zanna_add_basic_call_tests()
zanna_add_basic_integration_tests()
zanna_add_basic_namespace_tests()
zanna_add_e2e_codegen_tests()
zanna_add_golden_suite_tests()

# ── Milestone D unit tests (OOP) ────────────────────────────────────────
## Removed: test_oop_static_members (covered by integration tests)
## Removed: test_oop_properties (covered by TestPropertyBinding)

zanna_add_test(test_oop_overloads
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/oop/TestOverloads.cpp
        LIBS fe_basic)

zanna_add_test(test_oop_inheritance
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/oop/TestInheritance.cpp
        LIBS fe_basic zanna_test_common)

## Removed: test_oop_destructor_dispose (syntax validation; semantics in TestDispose)

zanna_add_test(test_oop_destructor_chain
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/oop/TestDestructorChain.cpp
        LIBS fe_basic zanna_test_common)

zanna_add_test(test_oop_dispose
        ${CMAKE_CURRENT_SOURCE_DIR}/unit/oop/TestDispose.cpp
        LIBS fe_basic zanna_test_common)

# Shell script tests - skip on Windows
if (NOT WIN32)
    add_test(NAME basic_repros
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_basic_repros.sh
            $<TARGET_FILE:zanna>)
    set_tests_properties(basic_repros
            PROPERTIES
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            LABELS smoke)
endif ()

# =============================================================================
# Test Labels Configuration
# =============================================================================
# Labels are assigned automatically by _zanna_assign_test_label() in
# TestHelpers.cmake, called from zanna_add_ctest() and zanna_add_test().
# All tests should use those functions for registration. The few remaining
# raw add_test() calls (shell scripts with WORKING_DIRECTORY) have explicit
# LABELS properties set at their definition site.
#
# Use `ctest -L <label>` to run tests with a specific label.
# Use `ctest --print-labels` to see all available labels.
# Available labels: basic, bytecode, il, vm, runtime, codegen, oop, golden,
#   e2e, examples, fuzz, audit, ilopt, conformance, smoke, tools, unit, zia,
#   namespace, perf, slow, tui
# =============================================================================

# Demo compilation tests removed — covered by ARM64 native link tests in e2e/CMakeLists.txt

# =============================================================================
# Zia Runtime Tests
# These tests verify the Zanna runtime API functions work correctly via Zia.
# Each test outputs "RESULT: ok" on success.
# =============================================================================
set(ZIA_RUNTIME_TESTS_DIR "${CMAKE_SOURCE_DIR}/src/tests/fixtures/runtime")
set(ZIA_RUNTIME_TESTS
        # ADR 0249-0253 runtime completions. Each is written on
        # Zanna.Testing.Suite, so these also exercise the harness itself.
        test_random_state
        test_playback_primitives
        test_text_table
        test_runtime_completions
        test_mesh3d_append
        test_math
        test_string
        test_bits
        test_list
        test_map
        test_collections
        test_bytes
        test_datetime
        test_random
        test_crypto
        test_graphics
        test_terminal
        test_environment
        test_diagnostics
        test_file
        test_gui
        test_bugfix_text
        test_bugfix_crypto
        test_bugfix_string
        test_bugfix_boolyn
        test_bugfix_html
        test_bugfix_bool_returns
        test_bugfix_game_heap
        test_bugfix_ring
        test_bugfix_saynum
        test_bugfix_random_new
        test_bugfix_zia_new
        test_sprite_flip
        test_sprite_png_load
        test_png_io
        test_png_formats
        test_collision_layers
        test_tilemap_collision
        test_tilemap_layers
        test_physics_joints
        test_dialogue
        test_tilemap_io
        test_iterator
        test_audio_api
        test_network_api
        test_threading_api
        test_concmap
        test_spritesheet
        test_physics2d_full
        test_json_stream
        test_action_api
        test_parallel_api
        test_chord_api
        test_quaternion
        test_spline
        test_serialize_unified
        test_weakref_gc
        test_async_promise
        test_intmap
        test_binbuf
        test_hash_fast
        test_datetime_parse
        test_string_like
        test_bytes_binary
        test_map_typed
        test_list_sort
        test_bytes_nullbyte
        test_final_const_expr
        test_namespace_length_collision
        test_bugfix_modvar_retain
        test_bugfix_loop_alloca
)

foreach (test_name IN LISTS ZIA_RUNTIME_TESTS)
    set(test_file "${ZIA_RUNTIME_TESTS_DIR}/${test_name}.zia")
    if (EXISTS "${test_file}")
        add_test(NAME zia_runtime_${test_name}
                COMMAND $<TARGET_FILE:zia> "${test_file}"
                WORKING_DIRECTORY "${ZIA_RUNTIME_TESTS_DIR}")
        set_tests_properties(zia_runtime_${test_name} PROPERTIES
                PASS_REGULAR_EXPRESSION "RESULT: ok"
                LABELS "runtime;zia"
                TIMEOUT 30)
    endif ()
endforeach ()

# =============================================================================
# Zia Runtime API, Audit, and Sweep Suites
# These suites are intentionally discovered from their directories because they
# are broad hardening sweeps rather than curated language chapters.
# =============================================================================
set(ZIA_RT_API_TESTS_DIR "${CMAKE_SOURCE_DIR}/src/tests/fixtures/rt_api")
file(GLOB ZIA_RT_API_TEST_FILES CONFIGURE_DEPENDS "${ZIA_RT_API_TESTS_DIR}/*.zia")
foreach (test_file IN LISTS ZIA_RT_API_TEST_FILES)
    get_filename_component(test_name "${test_file}" NAME_WE)
    add_test(NAME zia_rt_api_${test_name}
            COMMAND $<TARGET_FILE:zia> "${test_file}"
            WORKING_DIRECTORY "${ZIA_RT_API_TESTS_DIR}")
    set_tests_properties(zia_rt_api_${test_name} PROPERTIES
            LABELS "runtime;zia;rt_api"
            TIMEOUT 30)
endforeach ()

zanna_add_ctest(basic_rt_api_test_scene
        $<TARGET_FILE:zanna> front basic -run "${ZIA_RT_API_TESTS_DIR}/test_scene.bas")
set_tests_properties(basic_rt_api_test_scene PROPERTIES
        LABELS "runtime;basic;rt_api"
        TIMEOUT 30)

set(ZIA_AUDIT_TESTS_DIR "${CMAKE_SOURCE_DIR}/src/tests/fixtures/zia_audit")
file(GLOB ZIA_AUDIT_TEST_FILES CONFIGURE_DEPENDS "${ZIA_AUDIT_TESTS_DIR}/*.zia")
foreach (test_file IN LISTS ZIA_AUDIT_TEST_FILES)
    get_filename_component(test_name "${test_file}" NAME_WE)
    add_test(NAME zia_audit_${test_name}
            COMMAND $<TARGET_FILE:zia> "${test_file}"
            WORKING_DIRECTORY "${ZIA_AUDIT_TESTS_DIR}")
    set_tests_properties(zia_audit_${test_name} PROPERTIES
            LABELS "zia;audit"
            TIMEOUT 30)
endforeach ()

set(ZIA_SWEEP_TESTS_DIR "${CMAKE_SOURCE_DIR}/src/tests/fixtures/zia_sweep")
file(GLOB ZIA_SWEEP_TEST_FILES CONFIGURE_DEPENDS "${ZIA_SWEEP_TESTS_DIR}/*.zia")
foreach (test_file IN LISTS ZIA_SWEEP_TEST_FILES)
    get_filename_component(test_name "${test_file}" NAME_WE)
    if (NOT test_name STREQUAL "_support")
        add_test(NAME zia_sweep_${test_name}
                COMMAND $<TARGET_FILE:zia> "${test_file}"
                WORKING_DIRECTORY "${ZIA_SWEEP_TESTS_DIR}")
        set_tests_properties(zia_sweep_${test_name} PROPERTIES
                LABELS "zia;sweep"
                TIMEOUT 30)
    endif ()
endforeach ()

# =============================================================================
# Zia Language Feature Tests
# These tests verify Zia language features work correctly.
# Each test outputs "RESULT: ok" on success.
# =============================================================================
set(ZIA_LANG_TESTS_DIR "${CMAKE_SOURCE_DIR}/src/tests/fixtures/zia_runtime")
# Note: Some tests excluded due to environment requirements:
# - 04_environment, 17_time_env: require command-line arguments
# - 07_terminal_input: requires interactive input
# - 09_math_rnd: requires specific Zanna namespace bindings
set(ZIA_LANG_TESTS
        01_terminal_math_random
        02_strings
        03_collections
        05_optionals_match
        06_control_flow
        08_string_length
        10_for_break_continue
        11_recursion_factorial
        12_list_map_stats
        13_optional_chain
        14_class_methods
        15_generics_annotations
        16_string_building
        18_numeric_algorithms
        19_lambdas
        20_guard_error_handling
        21_inheritance_deep
        22_string_interpolation
        23_pattern_matching_advanced
        24_generics_advanced
        25_interfaces
        26_modules_namespaces
        27_type_coercion
        28_numeric_edge_cases
        29_control_flow_advanced
        30_struct_types
        31_typed_catch_list_shorthand
        32_overloading_inheritance
        33_enum_runtime
        34_async_functions
        35_optional_primitive_narrowing
        36_class_properties
        37_string_instance_methods
        38_deinit_bindings
        39_named_arguments
        40_variadic_functions
        41_runtime_reference_types
        42_try_catch_promises
        43_alpha_hardening
        44_language_promises
        45_runtime_api_conformance
        46_local_fixed_arrays_defer
        47_typed_runtime_collections
        48_optional_method_chain
        49_aggregate_final_initializers
        50_world_projection_runtime
        51_struct_return_abi
        52_path_datadir
)

foreach (test_name IN LISTS ZIA_LANG_TESTS)
    set(test_file "${ZIA_LANG_TESTS_DIR}/${test_name}.zia")
    if (EXISTS "${test_file}")
        add_test(NAME zia_lang_${test_name}
                COMMAND $<TARGET_FILE:zia> "${test_file}"
                WORKING_DIRECTORY "${ZIA_LANG_TESTS_DIR}")
        set_tests_properties(zia_lang_${test_name} PROPERTIES
                PASS_REGULAR_EXPRESSION "RESULT: ok"
                LABELS "zia"
                TIMEOUT 30)
    endif ()
endforeach ()

set(ZIA_ZANNA_RUN_O1_REGRESSION_TESTS
        18_numeric_algorithms
        31_typed_catch_list_shorthand
        42_try_catch_promises
        43_alpha_hardening
        44_language_promises
)

foreach (test_name IN LISTS ZIA_ZANNA_RUN_O1_REGRESSION_TESTS)
    set(test_file "${ZIA_LANG_TESTS_DIR}/${test_name}.zia")
    if (EXISTS "${test_file}")
        add_test(NAME zanna_run_o1_zia_${test_name}
                COMMAND $<TARGET_FILE:zanna> run "${test_file}"
                WORKING_DIRECTORY "${ZIA_LANG_TESTS_DIR}")
        set_tests_properties(zanna_run_o1_zia_${test_name} PROPERTIES
                PASS_REGULAR_EXPRESSION "RESULT: ok"
                LABELS "zia;zanna;optimization"
                TIMEOUT 30)
    endif ()
endforeach ()

if (NOT WIN32)
    add_test(NAME native_run_zia_42_try_catch_promises
            COMMAND ${CMAKE_COMMAND}
            -DZANNA_EXE=$<TARGET_FILE:zanna>
            -DTEST_FILE=${ZIA_LANG_TESTS_DIR}/42_try_catch_promises.zia
            -DOUT_EXE=${CMAKE_BINARY_DIR}/zia_42_try_catch_promises_native
            -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_zia_native_run.cmake
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(native_run_zia_42_try_catch_promises PROPERTIES
            LABELS "zia;native_run"
            TIMEOUT 60)

    add_test(NAME native_run_zia_43_alpha_hardening
            COMMAND ${CMAKE_COMMAND}
            -DZANNA_EXE=$<TARGET_FILE:zanna>
            -DTEST_FILE=${ZIA_LANG_TESTS_DIR}/43_alpha_hardening.zia
            -DOUT_EXE=${CMAKE_BINARY_DIR}/zia_43_alpha_hardening_native
            -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_zia_native_run.cmake
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(native_run_zia_43_alpha_hardening PROPERTIES
            LABELS "zia;native_run"
            TIMEOUT 60)

    add_test(NAME native_run_zia_44_language_promises
            COMMAND ${CMAKE_COMMAND}
            -DZANNA_EXE=$<TARGET_FILE:zanna>
            -DTEST_FILE=${ZIA_LANG_TESTS_DIR}/44_language_promises.zia
            -DOUT_EXE=${CMAKE_BINARY_DIR}/zia_44_language_promises_native
            -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_zia_native_run.cmake
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(native_run_zia_44_language_promises PROPERTIES
            LABELS "zia;native_run"
            TIMEOUT 60)

    # Struct-return ABI coverage runs at both optimization extremes: the
    # historical aggregate-return miscompiles (xenoscape MoveResult, BUG-006
    # family) only surfaced in optimized native builds.
    foreach (opt_level IN ITEMS O0 O2)
        add_test(NAME native_run_zia_51_struct_return_abi_${opt_level}
                COMMAND ${CMAKE_COMMAND}
                -DZANNA_EXE=$<TARGET_FILE:zanna>
                -DTEST_FILE=${ZIA_LANG_TESTS_DIR}/51_struct_return_abi.zia
                -DOUT_EXE=${CMAKE_BINARY_DIR}/zia_51_struct_return_abi_native_${opt_level}
                -DOPT_FLAG=-${opt_level}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_zia_native_run.cmake
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        set_tests_properties(native_run_zia_51_struct_return_abi_${opt_level} PROPERTIES
                LABELS "zia;native_run"
                TIMEOUT 60)
    endforeach ()
endif ()

if (WIN32)
    add_test(NAME native_run_windows_environment
            COMMAND ${CMAKE_COMMAND}
            -DZANNA_EXE=$<TARGET_FILE:zanna>
            -DTEST_WORK_DIR=${CMAKE_BINARY_DIR}/native_windows_environment
            -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_windows_native_environment.cmake
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(native_run_windows_environment PROPERTIES
            LABELS "zia;native_run;windows"
            TIMEOUT 90)
endif ()

if (ZANNA_BASH_EXECUTABLE)
    add_test(NAME zia_lang_34_async_functions_repeat
            COMMAND ${ZANNA_BASH_COMMAND}
            "src/tests/e2e/test_zia_async_functions_repeat.sh"
            $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}" 25)
    set_tests_properties(zia_lang_34_async_functions_repeat PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "zia;regression;requires_posix_shell"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            TIMEOUT 90)
endif ()

# =============================================================================
# Zia Namespace Bind Tests
# These tests verify the bind Zanna.* namespace import feature.
# Each test outputs "RESULT: ok" on success.
# =============================================================================
set(ZIA_BIND_NAMESPACE_TESTS_DIR "${CMAKE_SOURCE_DIR}/src/tests/fixtures/zia/bind_namespace")
set(ZIA_BIND_NAMESPACE_TESTS
        test_basic
        test_alias
        test_selective
        test_new
)

foreach (test_name IN LISTS ZIA_BIND_NAMESPACE_TESTS)
    set(test_file "${ZIA_BIND_NAMESPACE_TESTS_DIR}/${test_name}.zia")
    if (EXISTS "${test_file}")
        add_test(NAME zia_bind_namespace_${test_name}
                COMMAND $<TARGET_FILE:zia> "${test_file}"
                WORKING_DIRECTORY "${ZIA_BIND_NAMESPACE_TESTS_DIR}")
        set_tests_properties(zia_bind_namespace_${test_name} PROPERTIES
                PASS_REGULAR_EXPRESSION "RESULT: ok"
                LABELS "zia;namespace"
                TIMEOUT 30)
    endif ()
endforeach ()

# =============================================================================
# Zia App/Game Smoke Probes
# These exercise real example app/game module stacks with deterministic probes.
# =============================================================================
add_test(NAME zia_smoke_paint
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/examples/apps/paint/smoke_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/examples/apps/paint")
set_tests_properties(zia_smoke_paint PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;smoke"
        TIMEOUT 30)

add_test(NAME zia_smoke_paint_runtime_features
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/examples/apps/paint/runtime_features_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/examples/apps/paint")
set_tests_properties(zia_smoke_paint_runtime_features PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;smoke"
        TIMEOUT 30)

add_test(NAME zia_smoke_paint_canvas_capture
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/examples/apps/paint/palette_canvas_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/examples/apps/paint")
set_tests_properties(zia_smoke_paint_canvas_capture PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;smoke;requires_display"
        TIMEOUT 30)

add_test(NAME zia_smoke_zannastudio
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/smoke_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_smoke_zannastudio PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        FAIL_REGULAR_EXPRESSION "Framebuffer resize dimensions exceed maximum"
        LABELS "zia;smoke;requires_display"
        TIMEOUT 30)

add_test(NAME zia_smoke_zannastudio_project_compile
        COMMAND $<TARGET_FILE:zanna> build "${CMAKE_SOURCE_DIR}/src/zannastudio" -o "${CMAKE_BINARY_DIR}/zannastudio_smoke.il")
set_tests_properties(zia_smoke_zannastudio_project_compile PROPERTIES
        LABELS "zia;smoke;slow"
        TIMEOUT 180)

if (UNIX)
    add_test(NAME zannastudio_architecture_guard
            COMMAND "${CMAKE_SOURCE_DIR}/src/zannastudio/scripts/check_architecture.sh")
    set_tests_properties(zannastudio_architecture_guard PROPERTIES
            LABELS "zannastudio;architecture;lint"
            TIMEOUT 60)

    add_test(NAME zannastudio_architecture_guard_self_test
            COMMAND "${CMAKE_SOURCE_DIR}/src/zannastudio/scripts/test_check_architecture.sh")
    set_tests_properties(zannastudio_architecture_guard_self_test PROPERTIES
            LABELS "zannastudio;architecture;lint"
            TIMEOUT 30)
endif ()

add_test(NAME zannastudio_notification_policy
        COMMAND "${CMAKE_COMMAND}" -P
                "${CMAKE_SOURCE_DIR}/scripts/lint_zannastudio_notification_policy.cmake")
set_tests_properties(zannastudio_notification_policy PROPERTIES
        LABELS "zannastudio;shell;lint"
        TIMEOUT 30)

add_test(NAME zannastudio_modal_input_policy
        COMMAND "${CMAKE_COMMAND}" -P
                "${CMAKE_SOURCE_DIR}/scripts/lint_zannastudio_modal_input_policy.cmake")
set_tests_properties(zannastudio_modal_input_policy PROPERTIES
        LABELS "zannastudio;shell;lint"
        TIMEOUT 30)

add_test(NAME zia_zannastudio_phase0_phase1
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/phase0_phase1_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_phase0_phase1 PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;phase0;phase1"
        TIMEOUT 150)

add_test(NAME zia_zannastudio_phase2_phase3
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/phase2_phase3_probe.zia" -- --zia-bin $<TARGET_FILE:zia> --zanna-bin $<TARGET_FILE:zanna>
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_phase2_phase3 PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;phase2;phase3"
        # This end-to-end probe launches two compiler processes plus a debug
        # adapter.  Windows executable scanning can make cold starts outliers,
        # so keep the probe serial and leave headroom above its bounded waits.
        RUN_SERIAL TRUE
        TIMEOUT 120)

add_test(NAME zia_zannastudio_source_freshness
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/source_freshness_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_source_freshness PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;build;workspace"
        TIMEOUT 30)

add_test(NAME zia_zannastudio_editor_hot_path
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/editor_hot_path_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_editor_hot_path PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;editor;requires_display;perf"
        # This probe enforces two 250 ms wall-clock budgets.  CPU contention
        # from an otherwise unrelated parallel CTest worker can violate those
        # budgets even though the editor hot path is unchanged, so its timing
        # sample must own the host while it runs.
        RUN_SERIAL TRUE
        TIMEOUT 60)

add_test(NAME zia_zannastudio_palette_query
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/palette_query_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_palette_query PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;palette;requires_display"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_welcome
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/welcome_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_welcome PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;shell;requires_display"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_vector_icons
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/vector_icons_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_vector_icons PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;shell;requires_display"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_editor_buffer
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/editor_buffer_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_editor_buffer PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;editor;requires_display"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_undo_across_tabs
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/undo_across_tabs_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_undo_across_tabs PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;editor;requires_display"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_tab_switch
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/tab_switch_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_tab_switch PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;editor;perf;requires_display"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_split_editor
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/split_editor_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_split_editor PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;editor;shell;requires_display"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_native_tool_window
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/native_tool_window_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_native_tool_window PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;shell;windowing;requires_display"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_bottom_panel
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/bottom_panel_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_bottom_panel PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;shell;requires_display"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_tool_panel_toolbar
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/tool_panel_toolbar_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_tool_panel_toolbar PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;shell;console;diagnostics;requires_display"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_tool_panel_virtualization
        COMMAND $<TARGET_FILE:zia>
                "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/tool_panel_virtualization_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_tool_panel_virtualization PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;shell;perf;requires_display"
        TIMEOUT 90)

add_test(NAME zia_zannastudio_scene_editor_2d
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_editor_2d_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_editor_2d PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display;slow"
        # Source compilation and hidden-window rendering can exceed the normal
        # display-only runtime while unrelated CTest workers saturate the host.
        # Keep bounded headroom above the observed serial runtime.
        TIMEOUT 120)

add_test(NAME zia_zannastudio_scene_canvas_selection
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_canvas_selection_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_canvas_selection PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_scene_tile_tools
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_tile_tools_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_tile_tools PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_scene_tile_behavior
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_tile_behavior_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_tile_behavior PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_scene_object_preview
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_object_preview_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_object_preview PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_scene_camera_lighting
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_camera_lighting_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_camera_lighting PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_scene_schema_migration
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_schema_migration_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_schema_migration PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_scene_preview
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_preview_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_preview PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_scene_history
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_history_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_history PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_scene2d_history_palette
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene2d_history_palette_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene2d_history_palette PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 120)

add_test(NAME zia_zannastudio_scene_context_menus
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_context_menu_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_context_menus PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display;slow"
        TIMEOUT 120)

add_test(NAME zia_zannastudio_scene_dnd
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_dnd_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_dnd PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display;slow"
        TIMEOUT 180)

add_test(NAME zia_zannastudio_scene_asset_thumbnails
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_asset_thumbnail_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_asset_thumbnails PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 180)

add_test(NAME zia_zannastudio_scene_watch
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_watch_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_watch PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene"
        TIMEOUT 120)

add_test(NAME zia_zannastudio_scene_capacity
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_capacity_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_capacity PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;slow;requires_display"
        TIMEOUT 600)

add_test(NAME zia_zannastudio_scene_capacity_65k
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_capacity_65k_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_capacity_65k PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;slow;requires_display"
        TIMEOUT 1800)

add_test(NAME zia_zannastudio_scene_inline_rename
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_inline_rename_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_inline_rename PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display;slow"
        TIMEOUT 120)

add_test(NAME zia_zannastudio_scene2d_large_map
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene2d_large_map_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene2d_large_map PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 120)

add_test(NAME zia_zannastudio_scene2d_rulers_guides
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene2d_rulers_guides_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene2d_rulers_guides PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 120)

add_test(NAME zia_zannastudio_scene2d_layer_navigator
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene2d_layer_navigator_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene2d_layer_navigator PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 120)

add_test(NAME zia_zannastudio_scene2d_brush_preview
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene2d_brush_preview_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene2d_brush_preview PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 120)

add_test(NAME zia_zannastudio_scene2d_autotile_preview
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene2d_autotile_preview_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene2d_autotile_preview PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 120)

add_test(NAME zia_zannastudio_scene2d_object_draw_stack
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene2d_object_draw_stack_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene2d_object_draw_stack PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display;slow"
        TIMEOUT 150)

add_test(NAME zia_zannastudio_scene_game_view
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_game_view_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_game_view PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display;slow"
        TIMEOUT 150)

add_test(NAME zia_zannastudio_asset_library
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/asset_library_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_asset_library PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_scene_editor_3d
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_editor_3d_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_editor_3d PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        # This legacy aggregate compiles a very large Studio graph and repeats
        # contracts covered by the focused scene probes below. Keep it opt-in
        # with the other slow tests so routine platform validation is bounded.
        LABELS "zia;zannastudio;scene;requires_display;slow"
        TIMEOUT 600)
if (WIN32)
    # Both fixtures compile large Studio graphs and drive hidden native
    # rendering. Reserve the Windows runner so unrelated compiler probes cannot
    # starve them. A Release build of the especially large 3D fixture takes
    # about seven minutes on the reference host, so retain bounded headroom.
    set_tests_properties(
            zia_zannastudio_scene_editor_2d
            zia_zannastudio_scene_editor_3d
            PROPERTIES RUN_SERIAL TRUE)
    set_property(TEST zia_zannastudio_scene_editor_3d PROPERTY TIMEOUT 600)
endif ()

add_test(NAME zia_zannastudio_scene_perspective_viewport
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_perspective_viewport_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_perspective_viewport PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 150)

add_test(NAME zia_zannastudio_scene_orientation_navigator
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_orientation_navigator_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_orientation_navigator PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 150)

add_test(NAME zia_zannastudio_scene_hierarchy_affordances
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_hierarchy_affordances_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_hierarchy_affordances PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 150)

add_test(NAME zia_zannastudio_embed_channel
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/embed_channel_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_embed_channel PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;runtime"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_scene_collider_2d
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_collider_2d_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_collider_2d PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 120)

add_test(NAME zia_zannastudio_scene_settings_forms
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_settings_forms_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_settings_forms PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 150)

add_test(NAME zia_zannastudio_scene_gameplay_preview
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_gameplay_preview_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_gameplay_preview PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        SKIP_REGULAR_EXPRESSION "RESULT: skip"
        LABELS "zia;zannastudio;scene;requires_display;slow"
        TIMEOUT 150)

add_test(NAME zia_zannastudio_scene_gameplay_preview_2d
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_gameplay_preview_2d_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_gameplay_preview_2d PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        SKIP_REGULAR_EXPRESSION "RESULT: skip"
        LABELS "zia;zannastudio;scene;requires_display;slow"
        TIMEOUT 150)

add_test(NAME zia_zannastudio_scene_collider_authoring
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_collider_authoring_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_collider_authoring PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 150)

add_test(NAME zia_zannastudio_scene_material_library
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_material_library_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_material_library PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 150)

add_test(NAME zia_zannastudio_scene_bake_environment
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_bake_environment_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_bake_environment PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 300)

add_test(NAME zia_zannastudio_scene_prefab_instance
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_prefab_instance_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_prefab_instance PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 150)

add_test(NAME zia_zannastudio_scene_camera_authoring
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_camera_authoring_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_camera_authoring PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 150)

add_test(NAME zia_zannastudio_scene_light_authoring
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_light_authoring_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_light_authoring PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 150)

add_test(NAME zia_zannastudio_scene_rotation_ring
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_rotation_ring_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_rotation_ring PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 150)

add_test(NAME zia_zannastudio_scene_scale_plane
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_scale_plane_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_scale_plane PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 150)

add_test(NAME zia_zannastudio_scene_surface_drop
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_surface_drop_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_surface_drop PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display;slow"
        TIMEOUT 150)

add_test(NAME zia_zannastudio_scene_vertex_snap
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_vertex_snap_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_vertex_snap PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 150)

add_test(NAME zia_zannastudio_scene_terrain_authoring
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_terrain_authoring_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_terrain_authoring PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 180)

add_test(NAME zia_zannastudio_scene_material_batch
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_material_batch_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_material_batch PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 150)

add_test(NAME zia_zannastudio_scene_shaded_viewport
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_shaded_viewport_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_shaded_viewport PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 150)

add_test(NAME zia_zannastudio_scene_viewport_picking
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_viewport_picking_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_viewport_picking PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display"
        TIMEOUT 150)

add_test(NAME zia_zannastudio_scene_command_bar
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_command_bar_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_command_bar PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display;slow"
        TIMEOUT 150)

add_test(NAME zia_zannastudio_scene_inspector_topics
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_inspector_topics_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_inspector_topics PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;requires_display;slow"
        TIMEOUT 150)

add_test(NAME zia_zannastudio_scene_workspace_state
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_workspace_state_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_workspace_state PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene;persistence"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_scene_component_schema
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_component_schema_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_component_schema PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_scene_component_authoring
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scene_component_authoring_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scene_component_authoring PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scene"
        TIMEOUT 30)

add_test(NAME zia_zannastudio_navigation_safety
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/navigation_safety_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_navigation_safety PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;navigation;requires_display"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_safe_io
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/safe_io_native_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_safe_io PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;persistence"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_tree_move
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/tree_move_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_tree_move PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;explorer"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_recovery
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/recovery_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_recovery PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;session"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_autosave_job
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/autosave_job_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_autosave_job PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;persistence;perf"
        TIMEOUT 30)

add_test(NAME zia_zannastudio_settings_migration
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/settings_migration_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_settings_migration PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;session"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_keybinding_settings
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/keybinding_settings_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_keybinding_settings PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;settings;commands"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_project_replace
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/project_replace_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_project_replace PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;search"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_diff_engine
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/diff_view_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_diff_engine PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scm"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_editor_depth
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/editor_depth_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_editor_depth PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;editor"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_breadcrumb_index
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/breadcrumb_index_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_breadcrumb_index PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;editor;perf"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_shell_config
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/shell_config_probe.zia" -- --zanna-bin $<TARGET_FILE:zanna>
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_shell_config PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;shell"
        TIMEOUT 120)

add_test(NAME zia_zannastudio_palette_modes
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/palette_modes_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_palette_modes PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;palette;requires_display"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_scm_diff
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scm_diff_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scm_diff PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scm"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_scm_gutter
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scm_gutter_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scm_gutter PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scm;requires_display"
        RUN_SERIAL TRUE
        TIMEOUT 60)

add_test(NAME zia_zannastudio_intellisense
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/intellisense_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_intellisense PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;intellisense;requires_display;slow"
        TIMEOUT 420)

add_test(NAME zia_zannastudio_file_tree
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/file_tree_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_file_tree PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;file-tree;requires_display"
        TIMEOUT 90)

add_test(NAME zia_zannastudio_workspace_watcher
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/workspace_watcher_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_workspace_watcher PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;workspace-watcher;perf"
        TIMEOUT 30)

add_test(NAME zia_zannastudio_linked_workspace
        COMMAND ${CMAKE_COMMAND}
                -DZIA_EXECUTABLE=$<TARGET_FILE:zia>
                -DPROBE_SOURCE=${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/linked_workspace_probe.zia
                -DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/zannastudio-linked-workspace
                -P ${CMAKE_SOURCE_DIR}/src/tests/tools/ZannaStudioLinkedWorkspaceTests.cmake)
set_tests_properties(zia_zannastudio_linked_workspace PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;file-tree;workspace-watcher;requires_display"
        RUN_SERIAL TRUE
        TIMEOUT 45)

add_test(NAME zia_zannastudio_activity_bar
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/activity_bar_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_activity_bar PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;activity-bar;requires_display"
        TIMEOUT 30)

add_test(NAME zia_zannastudio_multi_root
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/multi_root_file_tree_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_multi_root PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;multi-root;requires_display;slow"
        # The probe completes in under a minute alone, but unrelated workers in
        # the full parallel Debug suite can double that runtime on Windows.
        TIMEOUT 180)

add_test(NAME zia_zannastudio_scm
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scm_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scm PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scm"
        TIMEOUT 30)

add_test(NAME zia_zannastudio_scm_history
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scm_history_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scm_history PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scm"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_scm_view
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/scm_view_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_scm_view PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;scm;shell;requires_display"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_terminal
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/terminal_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_terminal PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;terminal"
        TIMEOUT 30)

add_test(NAME zia_zannastudio_process_reaper
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/process_reaper_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_process_reaper PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;process"
        TIMEOUT 30)

add_test(NAME zia_zannastudio_terminal_open
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/terminal_open_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_terminal_open PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;terminal;requires_display"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_terminal_hidden_start
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/terminal_hidden_start_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_terminal_hidden_start PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;terminal;requires_display"
        TIMEOUT 30)

add_test(NAME zia_zannastudio_terminal_render
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/terminal_render_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_terminal_render PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;terminal;requires_display"
        RUN_SERIAL TRUE
        TIMEOUT 60)

add_test(NAME zia_zannastudio_terminal_altscreen
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/terminal_altscreen_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_terminal_altscreen PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;terminal;requires_display"
        TIMEOUT 30)

add_test(NAME zia_zannastudio_terminal_apps
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/terminal_apps_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_terminal_apps PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;terminal;requires_display;platform"
        RUN_SERIAL TRUE
        TIMEOUT 45)

add_test(NAME zia_zannastudio_terminal_multi
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/terminal_multi_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_terminal_multi PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;terminal;requires_display"
        TIMEOUT 30)

add_test(NAME zia_zannastudio_context_menu
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/context_menu_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_context_menu PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;context-menu;requires_display"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_syntax_render
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/syntax_render_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_syntax_render PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;syntax;requires_display"
        TIMEOUT 30)

add_test(NAME zia_zannastudio_formatting
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/formatting_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_formatting PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;format"
        TIMEOUT 30)

add_test(NAME zia_zannastudio_text_builders
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/text_builder_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_text_builders PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;format;perf"
        TIMEOUT 30)

add_test(NAME zia_zannastudio_edge_case_corpus
        COMMAND $<TARGET_FILE:zia>
                "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/edge_case_corpus_probe.zia"
                -- --corpus-root
                "${CMAKE_SOURCE_DIR}/src/zannastudio/tests/corpus/edge_cases")
set_tests_properties(zia_zannastudio_edge_case_corpus PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;corpus"
        TIMEOUT 90)

add_test(NAME zia_zannastudio_diagnostic_stream
        COMMAND $<TARGET_FILE:zia>
                "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/diagnostic_stream_probe.zia")
set_tests_properties(zia_zannastudio_diagnostic_stream PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;build;diagnostics"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_runtime_policy_perf
        COMMAND $<TARGET_FILE:zia>
                "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/runtime_policy_perf_probe.zia")
set_tests_properties(zia_zannastudio_runtime_policy_perf PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;perf;lifecycle"
        TIMEOUT 30)

add_test(NAME zia_zannastudio_platform_signoff_manifest
        COMMAND $<TARGET_FILE:zia>
                "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/platform_signoff_manifest_probe.zia"
                -- --manifest "${CMAKE_SOURCE_DIR}/src/zannastudio/tests/platform_signoff.tsv")
set_tests_properties(zia_zannastudio_platform_signoff_manifest PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;platform;signoff"
        TIMEOUT 30)

add_test(NAME zia_zannastudio_debug
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/debug_probe.zia" -- --zanna-bin $<TARGET_FILE:zanna>
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_debug PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;debug"
        RUN_SERIAL TRUE
        TIMEOUT 240)

add_test(NAME zia_zannastudio_debug_vars
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/debug_vars_probe.zia" -- --zanna-bin $<TARGET_FILE:zanna>
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_debug_vars PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;debug"
        RUN_SERIAL TRUE
        TIMEOUT 120)

add_test(NAME zia_zannastudio_debug_fields
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/debug_fields_probe.zia" -- --zanna-bin $<TARGET_FILE:zanna>
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_debug_fields PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;debug"
        RUN_SERIAL TRUE
        TIMEOUT 120)

add_test(NAME zia_zannastudio_debug_tree
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/debug_tree_probe.zia" -- --zanna-bin $<TARGET_FILE:zanna>
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_debug_tree PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;debug"
        RUN_SERIAL TRUE
        TIMEOUT 120)

add_test(NAME zia_zannastudio_debug_watch_toolbar
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/debug_watch_toolbar_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_debug_watch_toolbar PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;debug;shell;requires_display"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_debug_tool_surfaces
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/debug_tool_surfaces_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_debug_tool_surfaces PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;debug;shell;console;requires_display"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_run_debug_view
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/run_debug_view_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_run_debug_view PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;activity-bar;debug;shell;requires_display"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_semantic_tokens
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/semantic_tokens_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_semantic_tokens PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;semantic;requires_display"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_language_service_sync
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/language_service_sync_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_language_service_sync PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;semantic;requires_display"
        RUN_SERIAL TRUE
        TIMEOUT 90)

add_test(NAME zia_zannastudio_basic_query_job
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/basic_query_job_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_basic_query_job PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;semantic;threads"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_shutdown_state
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/shutdown_state_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_shutdown_state PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;lifecycle;threads"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_basic_async_controllers
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/basic_async_controller_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_basic_async_controllers PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;semantic;threads;requires_display"
        RUN_SERIAL TRUE
        TIMEOUT 90)

add_test(NAME zia_zannastudio_diagnostic_actions
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/diagnostic_action_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_diagnostic_actions PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;semantic;threads;requires_display"
        RUN_SERIAL TRUE
        TIMEOUT 90)

add_test(NAME zia_zannastudio_basic_workspace_query
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/basic_workspace_query_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_basic_workspace_query PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;semantic;threads;slow"
        TIMEOUT 90)

add_test(NAME zia_zannastudio_basic_workspace_commands
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/basic_workspace_command_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_basic_workspace_commands PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;semantic;threads;requires_display;slow"
        RUN_SERIAL TRUE
        TIMEOUT 90)

add_test(NAME zia_zannastudio_zia_workspace_commands
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/zia_workspace_command_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_zia_workspace_commands PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;semantic;threads;requires_display"
        RUN_SERIAL TRUE
        TIMEOUT 120)

add_test(NAME zia_zannastudio_workspace_edit_preview
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/workspace_edit_preview_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_workspace_edit_preview PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;semantic;requires_display"
        TIMEOUT 60)

add_test(NAME zia_zannastudio_paint_damage
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/paint_damage_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_paint_damage PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;perf;requires_display"
        TIMEOUT 90)

add_test(NAME zia_zannastudio_console_search
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes/console_search_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/zannastudio/src/probes")
set_tests_properties(zia_zannastudio_console_search PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;zannastudio;console;search;requires_display"
        TIMEOUT 420)

# Overlay-composition regressions first observed on Metal (NL-ZANNA-001/002).
# Each runs the checked-in reproduction on the platform's default backend and
# passes only on the corrected observable output: source-over blending for
# translucent overlay rects, and per-draw identity for same-size AA text.
add_test(NAME zia_regress_overlay_alpha_blend
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/tests/fixtures/zia_regress/overlay_alpha_repro.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/tests/fixtures/zia_regress")
set_tests_properties(zia_regress_overlay_alpha_blend PROPERTIES
        PASS_REGULAR_EXPRESSION "alpha50=9[4-8],6[2-6],3[0-4]"
        LABELS "zia;graphics3d"
        TIMEOUT 120)
add_test(NAME zia_regress_overlay_aa_text_identity
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/src/tests/fixtures/zia_regress/overlay_aa_text_repro.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/src/tests/fixtures/zia_regress")
set_tests_properties(zia_regress_overlay_aa_text_identity PROPERTIES
        PASS_REGULAR_EXPRESSION "top-green-pixels=[1-9]"
        LABELS "zia;graphics3d"
        TIMEOUT 120)

# Software-rasterizer determinism gate for the backend-conformance harness:
# renders the fixed conformance scene twice and requires byte-identical output.
# GPU backends are compared by hand via scripts/run_backend_conformance.sh.
if (ZANNA_BASH_EXECUTABLE)
    add_test(NAME zia_graphics_conformance_sw_determinism
            COMMAND ${ZANNA_BASH_COMMAND} "${CMAKE_SOURCE_DIR}/scripts/run_backend_conformance.sh"
                    --self-check --zia $<TARGET_FILE:zia>
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(zia_graphics_conformance_sw_determinism PROPERTIES
            PASS_REGULAR_EXPRESSION "RESULT: ok"
            LABELS "zia;smoke;graphics3d;requires_posix_shell"
            TIMEOUT 180)

    # Cross-backend visual conformance: the platform GPU backend must match the
    # software golden. This is the only automated gate on GPU colour correctness,
    # and while it carried the `slow` label (so `-LE slow` hid it from the default
    # run) an OpenGL display-transform regression shipped against a green suite —
    # the backend re-encoded already-encoded colour and every default-lane test
    # still passed. It is a default-lane test now. `--postfx` matters: the tone
    # curve is where that bug lived, so the no-postfx variant alone would not have
    # caught it. Like the other 100+ `requires_display` entries in this lane it
    # needs a display and a working GPU backend; the soak below stays opt-in.
    add_test(NAME zia_graphics_conformance_cross_backend
            COMMAND ${ZANNA_BASH_COMMAND} "${CMAKE_SOURCE_DIR}/scripts/run_backend_conformance.sh"
                    --postfx --zia $<TARGET_FILE:zia>
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(zia_graphics_conformance_cross_backend PROPERTIES
            PASS_REGULAR_EXPRESSION "RESULT: ok"
            LABELS "zia;graphics3d;gpu_smoke;requires_display;requires_posix_shell"
            TIMEOUT 600)

    add_test(NAME zia_graphics_3d_soak_short
            COMMAND ${ZANNA_BASH_COMMAND} "${CMAKE_SOURCE_DIR}/scripts/run_3d_soak.sh"
                    --zia $<TARGET_FILE:zia> --minutes 2
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(zia_graphics_3d_soak_short PROPERTIES
            PASS_REGULAR_EXPRESSION "RESULT: ok"
            LABELS "zia;graphics3d;soak;requires_posix_shell;slow"
            TIMEOUT 900)
endif ()

# Windows-only D3D11 render-to-texture readback regression (probe lives in the
# kept examples/3d tree).
if (WIN32)
    add_test(NAME zia_smoke_d3d11_rtt_readback
            COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/examples/3d/d3d11_rtt_readback_probe.zia"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(zia_smoke_d3d11_rtt_readback PROPERTIES
            PASS_REGULAR_EXPRESSION "RESULT: ok backend=d3d11"
            LABELS "zia;smoke;graphics3d;requires_display;d3d11"
            ENVIRONMENT "ZANNA_3D_BACKEND=d3d11"
            RESOURCE_LOCK zanna_display
            TIMEOUT 120)
endif ()

add_test(NAME zia_smoke_chess
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/examples/games/chess/smoke_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/examples/games/chess")
set_tests_properties(zia_smoke_chess PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;smoke"
        TIMEOUT 30)

add_test(NAME zia_smoke_chess_release
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/examples/games/chess/release_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/examples/games/chess")
set_tests_properties(zia_smoke_chess_release PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;smoke"
        TIMEOUT 30)

add_test(NAME zia_smoke_chess_layout
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/examples/games/chess/layout_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/examples/games/chess")
set_tests_properties(zia_smoke_chess_layout PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;smoke"
        TIMEOUT 30)

add_test(NAME zia_smoke_chess_board_render
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/examples/games/chess/board_render_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/examples/games/chess")
set_tests_properties(zia_smoke_chess_board_render PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;smoke;requires_display"
        TIMEOUT 30)

add_test(NAME zia_smoke_crackman
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/examples/games/crackman/smoke_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/examples/games/crackman")
set_tests_properties(zia_smoke_crackman PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;smoke"
        TIMEOUT 30)

add_test(NAME zia_smoke_crackman_movement
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/examples/games/crackman/movement_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/examples/games/crackman")
set_tests_properties(zia_smoke_crackman_movement PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;smoke"
        TIMEOUT 30)

add_test(NAME zia_smoke_crackman_ghost_stall
        COMMAND $<TARGET_FILE:zia> "${CMAKE_SOURCE_DIR}/examples/games/crackman/ghost_stall_probe.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/examples/games/crackman")
set_tests_properties(zia_smoke_crackman_ghost_stall PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;smoke"
        TIMEOUT 30)

add_test(NAME zanna_asset_bake_roundtrip
        COMMAND $<TARGET_FILE:zanna> asset bake
        "${CMAKE_SOURCE_DIR}/examples/3d/openworld_slice/assets/models/sphere_draco_eb.glb"
        "${CMAKE_BINARY_DIR}/asset_bake_test.vscn" --lods 2)
set_tests_properties(zanna_asset_bake_roundtrip PROPERTIES
        PASS_REGULAR_EXPRESSION "baked "
        LABELS "tools;graphics3d;smoke"
        TIMEOUT 60)

add_test(NAME zanna_asset_validate_report
        COMMAND $<TARGET_FILE:zanna> asset validate
        "${CMAKE_SOURCE_DIR}/examples/3d/openworld_slice/assets/models/skinned_agent_meshopt.glb")
set_tests_properties(zanna_asset_validate_report PROPERTIES
        PASS_REGULAR_EXPRESSION "skippedPrimitives"
        LABELS "tools;graphics3d;smoke"
        TIMEOUT 60)

add_test(NAME zanna_asset_bake_fidelity_report
        COMMAND ${CMAKE_COMMAND}
        -DZANNA_EXE=$<TARGET_FILE:zanna>
        -DINPUT=${CMAKE_SOURCE_DIR}/src/tests/fixtures/runtime/assets/gltf/asset_bake_lossy.gltf
        -DSOURCE_INPUT=${CMAKE_SOURCE_DIR}/examples/3d/openworld_slice/assets/models/quad_etc1s.glb
        -DDECODED_INPUT=${CMAKE_SOURCE_DIR}/src/tests/fixtures/runtime/texture_fidelity_decoded.obj
        -DOUTPUT=${CMAKE_BINARY_DIR}/asset_bake_fidelity_test.vscn
        -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_asset_bake_fidelity.cmake)
set_tests_properties(zanna_asset_bake_fidelity_report PROPERTIES
        LABELS "tools;graphics3d;smoke"
        TIMEOUT 60)


set(ZANNA_DISPLAY_TESTS
        zia_smoke_paint_canvas_capture
        zia_smoke_zannastudio
        zia_zannastudio_editor_hot_path
        zia_zannastudio_intellisense
        zia_zannastudio_file_tree
        zia_zannastudio_activity_bar
        zia_zannastudio_multi_root
        zia_zannastudio_scm_view
        zia_zannastudio_terminal_open
        zia_zannastudio_terminal_hidden_start
        zia_zannastudio_terminal_multi
        zia_zannastudio_bottom_panel
        zia_zannastudio_tool_panel_toolbar
        zia_zannastudio_scene_editor_2d
        zia_zannastudio_scene_canvas_selection
        zia_zannastudio_scene_tile_tools
        zia_zannastudio_scene_editor_3d
        zia_zannastudio_scene_rotation_ring
        zia_zannastudio_scene_scale_plane
        zia_zannastudio_scene_surface_drop
        zia_zannastudio_scene_vertex_snap
        zia_zannastudio_scene_material_batch
        zia_zannastudio_scene_shaded_viewport
        zia_zannastudio_scene_viewport_picking
        zia_zannastudio_debug_watch_toolbar
        zia_zannastudio_debug_tool_surfaces
        zia_zannastudio_run_debug_view
        zia_zannastudio_split_editor
        zia_zannastudio_console_search)
set_tests_properties(${ZANNA_DISPLAY_TESTS} PROPERTIES
        RESOURCE_LOCK zanna_display)

if (ZANNA_BASH_EXECUTABLE)
    add_test(NAME platform_policy_lint_strict
            COMMAND ${ZANNA_BASH_COMMAND} "scripts/lint_platform_policy.sh" --strict)
    set_tests_properties(platform_policy_lint_strict PROPERTIES
            PASS_REGULAR_EXPRESSION "Platform policy lint: clean"
            LABELS "tools;smoke;requires_posix_shell;slow"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            # A clean full-tree scan normally completes in about ten seconds on
            # Windows, but the complete parallel Debug suite can saturate process
            # startup and storage long enough to exceed a one-minute bound.
            TIMEOUT 180)

    add_test(NAME platform_policy_lint_empty_candidates
            COMMAND ${ZANNA_BASH_COMMAND} "src/tests/e2e/test_platform_policy_lint_empty_candidates.sh"
            "${CMAKE_SOURCE_DIR}")
    set_tests_properties(platform_policy_lint_empty_candidates PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "tools;smoke;requires_posix_shell"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            TIMEOUT 30)

    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME linux_ctest_label_policy
                COMMAND "${CMAKE_COMMAND}"
                "-DZANNA_CTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
                "-DZANNA_BUILD_DIR=${CMAKE_BINARY_DIR}"
                -P "${CMAKE_SOURCE_DIR}/src/tests/cmake/CheckLinuxCTestLabels.cmake")
        set_tests_properties(linux_ctest_label_policy PROPERTIES
                PASS_REGULAR_EXPRESSION "Linux CTest label policy: clean"
                LABELS "tools;unit;requires_linux"
                TIMEOUT 30)

        add_test(NAME linux_build_wrapper_symlink
                COMMAND ${ZANNA_BASH_COMMAND}
                "src/tests/e2e/test_linux_build_wrapper_symlink.sh"
                "${CMAKE_SOURCE_DIR}")
        set_tests_properties(linux_build_wrapper_symlink PROPERTIES
                PASS_REGULAR_EXPRESSION "PASS"
                LABELS "tools;unit;requires_linux;requires_posix_shell"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                TIMEOUT 30)
    endif ()
endif ()

set(VEC3_MUTATORS_PROBE "${CMAKE_SOURCE_DIR}/src/tests/e2e/vec3_mutators_probe.zia")
add_test(NAME zia_smoke_vec3_mutators
        COMMAND $<TARGET_FILE:zanna> run "${VEC3_MUTATORS_PROBE}")
set_tests_properties(zia_smoke_vec3_mutators PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;smoke;runtime"
        TIMEOUT 30)

set(PHYSICS3D_FIXED_STEP_PROBE "${CMAKE_SOURCE_DIR}/src/tests/e2e/physics3d_fixed_step_probe.zia")
add_test(NAME zia_smoke_physics3d_fixed_step
        COMMAND $<TARGET_FILE:zanna> run "${PHYSICS3D_FIXED_STEP_PROBE}")
set_tests_properties(zia_smoke_physics3d_fixed_step PROPERTIES
        PASS_REGULAR_EXPRESSION "RESULT: ok"
        LABELS "zia;smoke;runtime"
        TIMEOUT 30)

if (APPLE AND ZANNA_BASH_EXECUTABLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    add_test(NAME native_smoke_vec3_mutators_arm64
            COMMAND ${ZANNA_BASH_COMMAND} "${CMAKE_SOURCE_DIR}/src/tests/e2e/test_vec3_mutators_native.sh"
            $<TARGET_FILE:zanna> "${CMAKE_SOURCE_DIR}")
    set_tests_properties(native_smoke_vec3_mutators_arm64 PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "zia;smoke;native-link;slow"
            TIMEOUT 60)

    add_test(NAME native_smoke_physics3d_fixed_step_arm64
            COMMAND ${ZANNA_BASH_COMMAND} "${CMAKE_SOURCE_DIR}/src/tests/e2e/test_physics3d_fixed_step_native.sh"
            $<TARGET_FILE:zanna> "${CMAKE_SOURCE_DIR}")
    set_tests_properties(native_smoke_physics3d_fixed_step_arm64 PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "zia;smoke;native-link;slow"
            TIMEOUT 60)

    add_test(NAME native_smoke_achievement_draw_arm64
            COMMAND ${ZANNA_BASH_COMMAND} "${CMAKE_SOURCE_DIR}/src/tests/e2e/test_achievement_draw_native.sh"
            $<TARGET_FILE:zanna> "${CMAKE_SOURCE_DIR}")
    set_tests_properties(native_smoke_achievement_draw_arm64 PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "zia;smoke;native-link;requires_display;slow"
            RESOURCE_LOCK zanna_display
            TIMEOUT 60)

    add_test(NAME native_smoke_chess_ai_arm64
            COMMAND ${ZANNA_BASH_COMMAND} "${CMAKE_SOURCE_DIR}/src/tests/e2e/test_chess_ai_native.sh"
            $<TARGET_FILE:zanna> "${CMAKE_SOURCE_DIR}")
    set_tests_properties(native_smoke_chess_ai_arm64 PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "zia;smoke;native-link;slow"
            TIMEOUT 120)

    add_test(NAME native_smoke_crackman_movement_arm64
            COMMAND ${ZANNA_BASH_COMMAND} "${CMAKE_SOURCE_DIR}/src/tests/e2e/test_crackman_movement_native.sh"
            $<TARGET_FILE:zanna> "${CMAKE_SOURCE_DIR}")
    set_tests_properties(native_smoke_crackman_movement_arm64 PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "zia;smoke;native-link;slow"
            TIMEOUT 120)

    # Regression gate for the Zanna Studio IntelliSense fix: a codegen'd native
    # binary must force-load zia_editor_services so rt_zia_* resolve to the real engine
    # rather than the weak "unavailable" stubs. Fails before the fix.
    add_test(NAME native_smoke_zannastudio_completion_arm64
            COMMAND ${ZANNA_BASH_COMMAND} "${CMAKE_SOURCE_DIR}/src/tests/e2e/test_zannastudio_completion_native.sh"
            $<TARGET_FILE:zanna> "${CMAKE_SOURCE_DIR}")
    set_tests_properties(native_smoke_zannastudio_completion_arm64 PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "zia;smoke;native-link;slow"
            TIMEOUT 180)

    add_test(NAME native_smoke_zannastudio_safe_io_arm64
            COMMAND ${ZANNA_BASH_COMMAND} "${CMAKE_SOURCE_DIR}/src/tests/e2e/test_zannastudio_safe_io_native.sh"
            $<TARGET_FILE:zanna> "${CMAKE_SOURCE_DIR}" arm64)
    set_tests_properties(native_smoke_zannastudio_safe_io_arm64 PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "zia;zannastudio;smoke;native-link;slow"
            TIMEOUT 180)

    add_test(NAME native_smoke_zannastudio_basic_query_arm64
            COMMAND ${ZANNA_BASH_COMMAND} "${CMAKE_SOURCE_DIR}/src/tests/e2e/test_zannastudio_basic_query_native.sh"
            $<TARGET_FILE:zanna> "${CMAKE_SOURCE_DIR}" arm64)
    set_tests_properties(native_smoke_zannastudio_basic_query_arm64 PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "zia;zannastudio;semantic;threads;native-link;slow"
            TIMEOUT 180)
endif ()

if (ZANNA_BASH_EXECUTABLE AND
    CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    add_test(NAME native_smoke_zannastudio_completion_x64
            COMMAND "${CMAKE_COMMAND}" -E env ZANNA_NATIVE_TEST_ARCH=x64
            ${ZANNA_BASH_COMMAND} "${CMAKE_SOURCE_DIR}/src/tests/e2e/test_zannastudio_completion_native.sh"
            $<TARGET_FILE:zanna> "${CMAKE_SOURCE_DIR}")
    set_tests_properties(native_smoke_zannastudio_completion_x64 PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "zia;smoke;native-link;slow"
            TIMEOUT 180)

    add_test(NAME native_smoke_zannastudio_safe_io_x64
            COMMAND ${ZANNA_BASH_COMMAND} "${CMAKE_SOURCE_DIR}/src/tests/e2e/test_zannastudio_safe_io_native.sh"
            $<TARGET_FILE:zanna> "${CMAKE_SOURCE_DIR}" x64)
    set_tests_properties(native_smoke_zannastudio_safe_io_x64 PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "zia;zannastudio;smoke;native-link;slow"
            TIMEOUT 180)

    add_test(NAME native_smoke_zannastudio_basic_query_x64
            COMMAND ${ZANNA_BASH_COMMAND} "${CMAKE_SOURCE_DIR}/src/tests/e2e/test_zannastudio_basic_query_native.sh"
            $<TARGET_FILE:zanna> "${CMAKE_SOURCE_DIR}" x64)
    set_tests_properties(native_smoke_zannastudio_basic_query_x64 PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "zia;zannastudio;semantic;threads;native-link;slow"
            TIMEOUT 180)
endif ()

# =============================================================================
# Graphics3D Zia Integration Tests (headless — no display required)
# =============================================================================
set(G3D_TESTS_DIR "${CMAKE_SOURCE_DIR}/src/tests/fixtures/runtime")
# Byte-exact VSCN serialization goldens (v2/v5/v6/v7 documents built through
# the public API, compared against src/tests/golden/vscn/, plus reload
# byte-stability). Not slow: this is the default-gate scene-format tripwire.
add_test(NAME g3d_vscn_golden
        COMMAND $<TARGET_FILE:zanna> run "${CMAKE_SOURCE_DIR}/src/tests/fixtures/runtime/test_vscn_golden.zia"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(g3d_vscn_golden PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS"
        LABELS "runtime;graphics3d;golden"
        TIMEOUT 60)

set(G3D_ZIA_TESTS
        test_animcontroller3d
        test_g3d_api_symmetry
        test_g3d_api_symmetry2
        test_canvas3d_hello
        test_canvas3d_obj
        test_material3d_pbr
        test_graphics3d_scalar_set_aliases
        test_model3d
        test_model3d_load_asset
        test_scene3d_bindings
        test_physics3d_rotation
        test_collider3d_advanced
        test_physics3d_queries
        test_physics3d_ccd_toi
        test_physics3d_hull_kinematic
        test_anim_nav_upgrades
        test_canvas3d_viewmodel_sprite
        test_canvas3d_renderer_upgrades
        test_canvas3d_light_telemetry
        test_mesh_simplify
        test_fbx_ascii
        test_collisionevent3d
        test_navagent3d
        test_sound3d_objects
        test_g3d_scene_colliders
)
foreach (test_name IN LISTS G3D_ZIA_TESTS)
    set(test_file "${G3D_TESTS_DIR}/${test_name}.zia")
    if (EXISTS "${test_file}")
        add_test(NAME g3d_${test_name}
                COMMAND $<TARGET_FILE:zanna> run "${test_file}"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        set_tests_properties(g3d_${test_name} PROPERTIES
                PASS_REGULAR_EXPRESSION "PASS"
                LABELS "runtime;graphics3d"
                TIMEOUT 30)
    endif ()
endforeach ()

# Software-pinned: regresses the in-place CPU finalize chain (double-gamma
# wash on capture-after-present); GPU backends composite from scene targets
# and never had the defect.
set(G3D_CAPTURE_IDEMPOTENT "${G3D_TESTS_DIR}/test_g3d_capture_idempotent.zia")
if (EXISTS "${G3D_CAPTURE_IDEMPOTENT}")
    add_test(NAME g3d_test_g3d_capture_idempotent
            COMMAND $<TARGET_FILE:zanna> run "${G3D_CAPTURE_IDEMPOTENT}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_g3d_capture_idempotent PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(GAME3D_SURFACE_PROBE "${G3D_TESTS_DIR}/game3d_surface_probe.zia")
if (EXISTS "${GAME3D_SURFACE_PROBE}")
    add_test(NAME g3d_game3d_surface_probe
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_SURFACE_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_game3d_surface_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(CANVAS3D_SHADOW_DARKENS_PROBE "${G3D_TESTS_DIR}/test_canvas3d_shadow_darkens_probe.zia")
if (EXISTS "${CANVAS3D_SHADOW_DARKENS_PROBE}")
    add_test(NAME g3d_test_canvas3d_shadow_darkens_probe
            COMMAND $<TARGET_FILE:zanna> run "${CANVAS3D_SHADOW_DARKENS_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    # No backend override: pins SHADOW PIXELS on the platform's windowed
    # GPU backend (ZB-13 regression — Metal sampled shadow maps with the
    # wrong depth range and no smoke ever checked the pixels).
    set_tests_properties(g3d_test_canvas3d_shadow_darkens_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "RESULT: ok"
            LABELS "runtime;graphics3d;zia;requires_display"
            TIMEOUT 60)
endif ()

set(CANVAS3D_CAPTURE_PRESENT_PROBE "${G3D_TESTS_DIR}/test_canvas3d_capture_present_probe.zia")
if (EXISTS "${CANVAS3D_CAPTURE_PRESENT_PROBE}")
    add_test(NAME g3d_test_canvas3d_capture_present_probe
            COMMAND $<TARGET_FILE:zanna> run "${CANVAS3D_CAPTURE_PRESENT_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    # No backend override: the point is the platform's windowed GPU
    # present path (Metal direct-present on macOS) — E3 capture hardening.
    set_tests_properties(g3d_test_canvas3d_capture_present_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "RESULT: ok"
            LABELS "runtime;graphics3d;zia;requires_display"
            TIMEOUT 60)
endif ()

set(GAME3D_WORLD_PROBE "${G3D_TESTS_DIR}/test_game3d_world_probe.zia")
if (EXISTS "${GAME3D_WORLD_PROBE}")
    add_test(NAME g3d_test_game3d_world_probe
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_WORLD_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_world_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(GAME3D_RUNFRAMES_PROBE "${G3D_TESTS_DIR}/test_game3d_runframes_probe.zia")
if (EXISTS "${GAME3D_RUNFRAMES_PROBE}")
    add_test(NAME g3d_test_game3d_runframes_probe
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_RUNFRAMES_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_runframes_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(GAME3D_RUNFRAMES_CALLBACK_PROBE "${G3D_TESTS_DIR}/test_game3d_runframes_callback_probe.zia")
if (EXISTS "${GAME3D_RUNFRAMES_CALLBACK_PROBE}")
    add_test(NAME g3d_test_game3d_runframes_callback_probe
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_RUNFRAMES_CALLBACK_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_runframes_callback_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "RESULT: ok"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(GAME3D_RUNFIXED_CALLBACK_PROBE "${G3D_TESTS_DIR}/test_game3d_runfixed_callback_probe.zia")
if (EXISTS "${GAME3D_RUNFIXED_CALLBACK_PROBE}")
    add_test(NAME g3d_test_game3d_runfixed_callback_probe
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_RUNFIXED_CALLBACK_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_runfixed_callback_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "RESULT: ok"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
    if (NOT WIN32)
        add_test(NAME native_run_game3d_runfixed_callback_probe
                COMMAND ${CMAKE_COMMAND}
                -DZANNA_EXE=$<TARGET_FILE:zanna>
                -DTEST_FILE=${GAME3D_RUNFIXED_CALLBACK_PROBE}
                -DOUT_EXE=${CMAKE_BINARY_DIR}/game3d_runfixed_callback_probe_native
                -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_zia_native_run.cmake
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        set_tests_properties(native_run_game3d_runfixed_callback_probe PROPERTIES
                LABELS "zia;native_run;runtime;graphics3d;requires_display"
                ENVIRONMENT "ZANNA_3D_BACKEND=software"
                TIMEOUT 60)
    endif ()
endif ()

set(GAME3D_DESTROYED_HANDLE_REJECT_PROBE "${G3D_TESTS_DIR}/test_game3d_destroyed_handle_reject.zia")
if (EXISTS "${GAME3D_DESTROYED_HANDLE_REJECT_PROBE}")
    add_test(NAME g3d_test_game3d_destroyed_handle_reject
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_DESTROYED_HANDLE_REJECT_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_destroyed_handle_reject PROPERTIES
            PASS_REGULAR_EXPRESSION "Game3D.World3D.get_Frame: world is destroyed"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(GAME3D_DESTROYED_ENTITY_REJECT_PROBE "${G3D_TESTS_DIR}/test_game3d_destroyed_entity_reject.zia")
if (EXISTS "${GAME3D_DESTROYED_ENTITY_REJECT_PROBE}")
    add_test(NAME g3d_test_game3d_destroyed_entity_reject
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_DESTROYED_ENTITY_REJECT_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_destroyed_entity_reject PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS stale entity no-op"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(GAME3D_DOUBLE_DESPAWN_REJECT_PROBE "${G3D_TESTS_DIR}/test_game3d_double_despawn_reject.zia")
if (EXISTS "${GAME3D_DOUBLE_DESPAWN_REJECT_PROBE}")
    add_test(NAME g3d_test_game3d_double_despawn_reject
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_DOUBLE_DESPAWN_REJECT_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_double_despawn_reject PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS double despawn no-op"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(CANVAS3D_SYNTHETIC_INPUT_PROBE "${G3D_TESTS_DIR}/test_canvas3d_synthetic_input.zia")
if (EXISTS "${CANVAS3D_SYNTHETIC_INPUT_PROBE}")
    add_test(NAME g3d_test_canvas3d_synthetic_input
            COMMAND $<TARGET_FILE:zanna> run "${CANVAS3D_SYNTHETIC_INPUT_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_canvas3d_synthetic_input PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(GAME3D_CAMERA_CONTROLLERS_PROBE "${G3D_TESTS_DIR}/test_game3d_camera_controllers_probe.zia")
if (EXISTS "${GAME3D_CAMERA_CONTROLLERS_PROBE}")
    add_test(NAME g3d_test_game3d_camera_controllers_probe
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_CAMERA_CONTROLLERS_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_camera_controllers_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(GAME3D_CHARACTER_CONTROLLER_PROBE "${G3D_TESTS_DIR}/test_game3d_character_controller_probe.zia")
if (EXISTS "${GAME3D_CHARACTER_CONTROLLER_PROBE}")
    add_test(NAME g3d_test_game3d_character_controller_probe
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_CHARACTER_CONTROLLER_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_character_controller_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(GAME3D_THIRDPERSON_PROBE "${G3D_TESTS_DIR}/test_game3d_thirdperson_probe.zia")
if (EXISTS "${GAME3D_THIRDPERSON_PROBE}")
    add_test(NAME g3d_test_game3d_thirdperson_probe
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_THIRDPERSON_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_thirdperson_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(GAME3D_COMBAT_PROBE "${G3D_TESTS_DIR}/test_game3d_combat_probe.zia")
if (EXISTS "${GAME3D_COMBAT_PROBE}")
    add_test(NAME g3d_test_game3d_combat_probe
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_COMBAT_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_combat_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(INTERACT_FIXTURE "${G3D_TESTS_DIR}/test_game3d_interact_probe.zia")
if (EXISTS "${INTERACT_FIXTURE}")
    add_test(NAME g3d_test_game3d_interact_probe
            COMMAND $<TARGET_FILE:zanna> run "${INTERACT_FIXTURE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_interact_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(GAME3D_PROFILING_FIXTURE "${G3D_TESTS_DIR}/test_game3d_profiling_probe.zia")
if (EXISTS "${GAME3D_PROFILING_FIXTURE}")
    add_test(NAME g3d_test_game3d_profiling_probe
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_PROFILING_FIXTURE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_profiling_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(GAME_QUESTS_FIXTURE "${G3D_TESTS_DIR}/test_game_quests_probe.zia")
if (EXISTS "${GAME_QUESTS_FIXTURE}")
    add_test(NAME zia_test_game_quests_probe
            COMMAND $<TARGET_FILE:zanna> run "${GAME_QUESTS_FIXTURE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(zia_test_game_quests_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;zia"
            TIMEOUT 30)
endif ()

set(GAME3D_MINIMAP_FIXTURE "${G3D_TESTS_DIR}/test_game3d_minimap_probe.zia")
if (EXISTS "${GAME3D_MINIMAP_FIXTURE}")
    add_test(NAME g3d_test_game3d_minimap_probe
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_MINIMAP_FIXTURE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_minimap_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(GAME3D_PERSIST_FIXTURE "${G3D_TESTS_DIR}/test_game3d_persistence_probe.zia")
if (EXISTS "${GAME3D_PERSIST_FIXTURE}")
    add_test(NAME g3d_test_game3d_persistence_probe
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_PERSIST_FIXTURE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_persistence_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(GAME3D_CLOTH_FIXTURE "${G3D_TESTS_DIR}/test_game3d_cloth_probe.zia")
if (EXISTS "${GAME3D_CLOTH_FIXTURE}")
    add_test(NAME g3d_test_game3d_cloth_probe
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_CLOTH_FIXTURE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_cloth_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(GAME3D_AUDIO_IMM_FIXTURE "${G3D_TESTS_DIR}/test_game3d_audio_immersion_probe.zia")
if (EXISTS "${GAME3D_AUDIO_IMM_FIXTURE}")
    add_test(NAME g3d_test_game3d_audio_immersion_probe
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_AUDIO_IMM_FIXTURE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_audio_immersion_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia"
            ENVIRONMENT "ZANNA_3D_BACKEND=software;ZANNA_AUDIO_SILENT=1"
            TIMEOUT 30)
endif ()

set(GAME3D_AI_FIXTURE "${G3D_TESTS_DIR}/test_game3d_ai_probe.zia")
if (EXISTS "${GAME3D_AI_FIXTURE}")
    add_test(NAME g3d_test_game3d_ai_probe
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_AI_FIXTURE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_ai_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(FOOTSTEPS_FIXTURE "${G3D_TESTS_DIR}/test_game3d_footsteps_probe.zia")
if (EXISTS "${FOOTSTEPS_FIXTURE}")
    add_test(NAME g3d_test_game3d_footsteps_probe
            COMMAND $<TARGET_FILE:zanna> run "${FOOTSTEPS_FIXTURE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_footsteps_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(SURFACES_FIXTURE "${G3D_TESTS_DIR}/test_game3d_surfaces_probe.zia")
if (EXISTS "${SURFACES_FIXTURE}")
    add_test(NAME g3d_test_game3d_surfaces_probe
            COMMAND $<TARGET_FILE:zanna> run "${SURFACES_FIXTURE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_surfaces_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(TIMEOFDAY_FIXTURE "${G3D_TESTS_DIR}/test_timeofday3d_clock.zia")
if (EXISTS "${TIMEOFDAY_FIXTURE}")
    add_test(NAME g3d_test_timeofday3d_clock
            COMMAND $<TARGET_FILE:zanna> run "${TIMEOFDAY_FIXTURE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_timeofday3d_clock PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(SKY3D_FIXTURE "${G3D_TESTS_DIR}/test_sky3d_procedural.zia")
if (EXISTS "${SKY3D_FIXTURE}")
    add_test(NAME g3d_test_sky3d_procedural
            COMMAND $<TARGET_FILE:zanna> run "${SKY3D_FIXTURE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_sky3d_procedural PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(REFLECTION_PROBE_FIXTURE "${G3D_TESTS_DIR}/test_reflection_probe_capture.zia")
if (EXISTS "${REFLECTION_PROBE_FIXTURE}")
    add_test(NAME g3d_test_reflection_probe_capture
            COMMAND $<TARGET_FILE:zanna> run "${REFLECTION_PROBE_FIXTURE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_reflection_probe_capture PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(GAME3D_CINEMATICS_PROBE "${G3D_TESTS_DIR}/test_game3d_cinematics_probe.zia")
if (EXISTS "${GAME3D_CINEMATICS_PROBE}")
    add_test(NAME g3d_test_game3d_cinematics_probe
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_CINEMATICS_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_cinematics_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(GAME3D_PRESETS_PROBE "${G3D_TESTS_DIR}/test_game3d_presets_probe.zia")
if (EXISTS "${GAME3D_PRESETS_PROBE}")
    add_test(NAME g3d_test_game3d_presets_probe
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_PRESETS_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_presets_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(GAME3D_ASSETS_PROBE "${G3D_TESTS_DIR}/test_game3d_assets_probe.zia")
if (EXISTS "${GAME3D_ASSETS_PROBE}")
    add_test(NAME g3d_test_game3d_assets_probe
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_ASSETS_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_assets_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(GAME3D_PHYSICS_PROBE "${G3D_TESTS_DIR}/test_game3d_physics_probe.zia")
if (EXISTS "${GAME3D_PHYSICS_PROBE}")
    add_test(NAME g3d_test_game3d_physics_probe
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_PHYSICS_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_physics_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(GAME3D_COLLISION_PROBE "${G3D_TESTS_DIR}/test_game3d_collision_probe.zia")
if (EXISTS "${GAME3D_COLLISION_PROBE}")
    add_test(NAME g3d_test_game3d_collision_probe
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_COLLISION_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_collision_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(GAME3D_ANIM_PROBE "${G3D_TESTS_DIR}/test_game3d_anim_probe.zia")
if (EXISTS "${GAME3D_ANIM_PROBE}")
    add_test(NAME g3d_test_game3d_anim_probe
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_ANIM_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_anim_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(GAME3D_SOUND_PROBE "${G3D_TESTS_DIR}/test_game3d_sound_probe.zia")
if (EXISTS "${GAME3D_SOUND_PROBE}")
    add_test(NAME g3d_test_game3d_sound_probe
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_SOUND_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_sound_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(GAME3D_EFFECTS_PROBE "${G3D_TESTS_DIR}/test_game3d_effects_probe.zia")
if (EXISTS "${GAME3D_EFFECTS_PROBE}")
    add_test(NAME g3d_test_game3d_effects_probe
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_EFFECTS_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_effects_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(GAME3D_DOCS_SNIPPETS_PROBE "${G3D_TESTS_DIR}/test_game3d_docs_snippets.zia")
if (EXISTS "${GAME3D_DOCS_SNIPPETS_PROBE}")
    add_test(NAME g3d_test_game3d_docs_snippets
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_DOCS_SNIPPETS_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_docs_snippets PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(GRAPHICS3D_DOCS_SNIPPETS_PROBE "${G3D_TESTS_DIR}/test_graphics3d_docs_snippets.zia")
if (EXISTS "${GRAPHICS3D_DOCS_SNIPPETS_PROBE}")
    add_test(NAME g3d_test_graphics3d_docs_snippets
            COMMAND $<TARGET_FILE:zanna> run "${GRAPHICS3D_DOCS_SNIPPETS_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_graphics3d_docs_snippets PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(CANVAS3D_SHADOW_BUDGET_PROBE "${G3D_TESTS_DIR}/test_canvas3d_shadow_budget.zia")
if (EXISTS "${CANVAS3D_SHADOW_BUDGET_PROBE}")
    add_test(NAME g3d_test_canvas3d_shadow_budget
            COMMAND $<TARGET_FILE:zanna> run "${CANVAS3D_SHADOW_BUDGET_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_canvas3d_shadow_budget PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 60)
endif ()

set(CANVAS3D_VISUAL_POLISH_PROBE "${G3D_TESTS_DIR}/test_canvas3d_visual_polish.zia")
if (EXISTS "${CANVAS3D_VISUAL_POLISH_PROBE}")
    add_test(NAME g3d_test_canvas3d_visual_polish
            COMMAND $<TARGET_FILE:zanna> run "${CANVAS3D_VISUAL_POLISH_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_canvas3d_visual_polish PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 60)
endif ()

set(CANVAS3D_POSTFX_PARITY_PROBE "${G3D_TESTS_DIR}/test_canvas3d_postfx_parity.zia")
if (APPLE)
    set(CANVAS3D_POSTFX_PARITY_GPU_BACKEND "metal")
elseif (WIN32)
    set(CANVAS3D_POSTFX_PARITY_GPU_BACKEND "d3d11")
else ()
    set(CANVAS3D_POSTFX_PARITY_GPU_BACKEND "opengl")
endif ()
if (EXISTS "${CANVAS3D_POSTFX_PARITY_PROBE}")
    add_test(NAME g3d_test_canvas3d_postfx_parity
            COMMAND $<TARGET_FILE:zanna> run "${CANVAS3D_POSTFX_PARITY_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_canvas3d_postfx_parity PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 60)

    # ADR 0247: the same probe pins the display-transfer contract on the
    # platform GPU backend — its ACES anchors must match software's.
    add_test(NAME g3d_test_canvas3d_postfx_parity_gpu
            COMMAND $<TARGET_FILE:zanna> run "${CANVAS3D_POSTFX_PARITY_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_canvas3d_postfx_parity_gpu PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=${CANVAS3D_POSTFX_PARITY_GPU_BACKEND}"
            TIMEOUT 60)
endif ()

set(CANVAS3D_DOUBLE_SIDED_PROBE "${G3D_TESTS_DIR}/test_canvas3d_double_sided_lighting.zia")
if (EXISTS "${CANVAS3D_DOUBLE_SIDED_PROBE}")
    # ZB-21: double-sided geometry shades with the outward normal on both
    # sides, on every backend (software + the platform GPU backend).
    add_test(NAME g3d_test_canvas3d_double_sided_lighting
            COMMAND $<TARGET_FILE:zanna> run "${CANVAS3D_DOUBLE_SIDED_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_canvas3d_double_sided_lighting PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 60)

    add_test(NAME g3d_test_canvas3d_double_sided_lighting_gpu
            COMMAND $<TARGET_FILE:zanna> run "${CANVAS3D_DOUBLE_SIDED_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_canvas3d_double_sided_lighting_gpu PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=${CANVAS3D_POSTFX_PARITY_GPU_BACKEND}"
            TIMEOUT 60)
endif ()

set(GAME3D_WORLD_FOG_ORDER_PROBE "${G3D_TESTS_DIR}/test_game3d_world_fog_order.zia")
if (EXISTS "${GAME3D_WORLD_FOG_ORDER_PROBE}")
    # ADR 0248 (ZB-22): World3D.SetFog forwards (near, far, r, g, b) —
    # Canvas3D's order — and World3D.ClearFog turns fog back off.
    add_test(NAME g3d_test_game3d_world_fog_order
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_WORLD_FOG_ORDER_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_game3d_world_fog_order PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 60)
endif ()

set(CANVAS3D_POINT_SHADOWS_PROBE "${G3D_TESTS_DIR}/test_canvas3d_point_shadows.zia")
if (EXISTS "${CANVAS3D_POINT_SHADOWS_PROBE}")
    add_test(NAME g3d_test_canvas3d_point_shadows
            COMMAND $<TARGET_FILE:zanna> run "${CANVAS3D_POINT_SHADOWS_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_canvas3d_point_shadows PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 60)
    if (WIN32)
        add_test(NAME g3d_test_canvas3d_point_shadows_d3d11
                COMMAND $<TARGET_FILE:zia> "${CANVAS3D_POINT_SHADOWS_PROBE}"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        set_tests_properties(g3d_test_canvas3d_point_shadows_d3d11 PROPERTIES
                PASS_REGULAR_EXPRESSION "PASS"
                LABELS "runtime;graphics3d;zia;requires_display;d3d11"
                ENVIRONMENT "ZANNA_3D_BACKEND=d3d11"
                TIMEOUT 120)
    elseif (APPLE)
        # Metal advertises shadow_atlas_slots; run the same probe on the real
        # device so macOS point shadows have a default-lane regression net.
        add_test(NAME g3d_test_canvas3d_point_shadows_metal
                COMMAND $<TARGET_FILE:zanna> run "${CANVAS3D_POINT_SHADOWS_PROBE}"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        set_tests_properties(g3d_test_canvas3d_point_shadows_metal PROPERTIES
                PASS_REGULAR_EXPRESSION "PASS"
                LABELS "runtime;graphics3d;zia;requires_display;metal"
                ENVIRONMENT "ZANNA_3D_BACKEND=metal"
                TIMEOUT 120)
    endif ()
endif ()

# ADR 0309: render-target shadow inheritance — RT frames re-arm the last full
# pass's non-cascade slots instead of re-rendering them (opt-in).
set(CANVAS3D_RT_SHADOW_INHERIT_PROBE "${G3D_TESTS_DIR}/test_canvas3d_rt_shadow_inherit.zia")
if (EXISTS "${CANVAS3D_RT_SHADOW_INHERIT_PROBE}")
    add_test(NAME g3d_test_canvas3d_rt_shadow_inherit
            COMMAND $<TARGET_FILE:zanna> run "${CANVAS3D_RT_SHADOW_INHERIT_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_canvas3d_rt_shadow_inherit PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 60)
    if (WIN32)
        add_test(NAME g3d_test_canvas3d_rt_shadow_inherit_d3d11
                COMMAND $<TARGET_FILE:zia> "${CANVAS3D_RT_SHADOW_INHERIT_PROBE}"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        set_tests_properties(g3d_test_canvas3d_rt_shadow_inherit_d3d11 PROPERTIES
                PASS_REGULAR_EXPRESSION "PASS"
                LABELS "runtime;graphics3d;zia;requires_display;d3d11"
                ENVIRONMENT "ZANNA_3D_BACKEND=d3d11"
                TIMEOUT 120)
    elseif (APPLE)
        # Metal is where atlas-tile re-arm legality matters most: run the same
        # probe on the real device.
        add_test(NAME g3d_test_canvas3d_rt_shadow_inherit_metal
                COMMAND $<TARGET_FILE:zanna> run "${CANVAS3D_RT_SHADOW_INHERIT_PROBE}"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        set_tests_properties(g3d_test_canvas3d_rt_shadow_inherit_metal PROPERTIES
                PASS_REGULAR_EXPRESSION "PASS"
                LABELS "runtime;graphics3d;zia;requires_display;metal"
                ENVIRONMENT "ZANNA_3D_BACKEND=metal"
                TIMEOUT 120)
    endif ()
endif ()

# Backend telemetry surface (GAP-10): stats must move on the platform GPU
# backend and stay zero on software (no hook).
set(CANVAS3D_BACKEND_STATS_PROBE "${G3D_TESTS_DIR}/test_canvas3d_backend_stats.zia")
if (EXISTS "${CANVAS3D_BACKEND_STATS_PROBE}")
    add_test(NAME g3d_test_canvas3d_backend_stats
            COMMAND $<TARGET_FILE:zanna> run "${CANVAS3D_BACKEND_STATS_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_canvas3d_backend_stats PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 60)
    if (APPLE)
        add_test(NAME g3d_test_canvas3d_backend_stats_metal
                COMMAND $<TARGET_FILE:zanna> run "${CANVAS3D_BACKEND_STATS_PROBE}"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        set_tests_properties(g3d_test_canvas3d_backend_stats_metal PROPERTIES
                PASS_REGULAR_EXPRESSION "PASS"
                LABELS "runtime;graphics3d;zia;requires_display;metal"
                ENVIRONMENT "ZANNA_3D_BACKEND=metal"
                TIMEOUT 120)
    elseif (WIN32)
        add_test(NAME g3d_test_canvas3d_backend_stats_d3d11
                COMMAND $<TARGET_FILE:zanna> run "${CANVAS3D_BACKEND_STATS_PROBE}"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        set_tests_properties(g3d_test_canvas3d_backend_stats_d3d11 PROPERTIES
                PASS_REGULAR_EXPRESSION "PASS"
                LABELS "runtime;graphics3d;zia;requires_display;d3d11"
                ENVIRONMENT "ZANNA_3D_BACKEND=d3d11"
                TIMEOUT 120)
    endif ()
endif ()

set(CANVAS3D_RENDER_SCALE_PROBE "${G3D_TESTS_DIR}/test_canvas3d_render_scale.zia")
if (EXISTS "${CANVAS3D_RENDER_SCALE_PROBE}")
    add_test(NAME g3d_test_canvas3d_render_scale
            COMMAND $<TARGET_FILE:zanna> run "${CANVAS3D_RENDER_SCALE_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_canvas3d_render_scale PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 60)
    if (APPLE)
        add_test(NAME g3d_test_canvas3d_render_scale_metal
                COMMAND $<TARGET_FILE:zanna> run "${CANVAS3D_RENDER_SCALE_PROBE}"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        set_tests_properties(g3d_test_canvas3d_render_scale_metal PROPERTIES
                PASS_REGULAR_EXPRESSION "PASS"
                LABELS "runtime;graphics3d;zia;requires_display;metal"
                ENVIRONMENT "ZANNA_3D_BACKEND=metal"
                TIMEOUT 120)
    elseif (WIN32)
        add_test(NAME g3d_test_canvas3d_render_scale_d3d11
                COMMAND $<TARGET_FILE:zanna> run "${CANVAS3D_RENDER_SCALE_PROBE}"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        set_tests_properties(g3d_test_canvas3d_render_scale_d3d11 PROPERTIES
                PASS_REGULAR_EXPRESSION "PASS"
                LABELS "runtime;graphics3d;zia;requires_display;d3d11"
                ENVIRONMENT "ZANNA_3D_BACKEND=d3d11"
                TIMEOUT 120)
    elseif (UNIX)
        add_test(NAME g3d_test_canvas3d_render_scale_opengl
                COMMAND $<TARGET_FILE:zanna> run "${CANVAS3D_RENDER_SCALE_PROBE}"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        set_tests_properties(g3d_test_canvas3d_render_scale_opengl PROPERTIES
                PASS_REGULAR_EXPRESSION "PASS"
                LABELS "runtime;graphics3d;zia;requires_display;opengl"
                ENVIRONMENT "ZANNA_3D_BACKEND=opengl"
                TIMEOUT 120)
    endif ()
endif ()

set(CANVAS3D_QUALITY_PROFILES_PROBE "${G3D_TESTS_DIR}/test_canvas3d_quality_profiles.zia")
if (EXISTS "${CANVAS3D_QUALITY_PROFILES_PROBE}")
    add_test(NAME g3d_test_canvas3d_quality_profiles
            COMMAND $<TARGET_FILE:zanna> run "${CANVAS3D_QUALITY_PROFILES_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_test_canvas3d_quality_profiles PROPERTIES
            PASS_REGULAR_EXPRESSION "RESULT: ok"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(WALK_MIN_VISUAL_PROBE "${CMAKE_SOURCE_DIR}/examples/3d/walk_min_probe.zia")
if (EXISTS "${WALK_MIN_VISUAL_PROBE}")
    add_test(NAME g3d_walk_min_visual_probe
            COMMAND $<TARGET_FILE:zanna> run "${WALK_MIN_VISUAL_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_walk_min_visual_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "RESULT: ok"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(BOUNDED_NO_REGRESSION_PROBE "${CMAKE_SOURCE_DIR}/examples/3d/bounded_no_regression_probe.zia")
if (EXISTS "${BOUNDED_NO_REGRESSION_PROBE}")
    add_test(NAME g3d_bounded_no_regression_probe
            COMMAND $<TARGET_FILE:zanna> run "${BOUNDED_NO_REGRESSION_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/examples/3d")
    set_tests_properties(g3d_bounded_no_regression_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "RESULT: ok .*byte_exact=1.*state_exact=1.*scale_flags_off=1"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

set(GAME3D_SHOWCASE_PROBE "${CMAKE_SOURCE_DIR}/examples/3d/game3d_showcase/showcase.zia")
if (EXISTS "${GAME3D_SHOWCASE_PROBE}")
    add_test(NAME g3d_game3d_showcase
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_SHOWCASE_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_game3d_showcase PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 45)
endif ()

set(GAME3D_HELLO_PROBE "${CMAKE_SOURCE_DIR}/examples/3d/game3d_hello.zia")
if (EXISTS "${GAME3D_HELLO_PROBE}")
    add_test(NAME g3d_game3d_hello
            COMMAND $<TARGET_FILE:zanna> run "${GAME3D_HELLO_PROBE}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(g3d_game3d_hello PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)
endif ()

add_test(NAME g3d_game3d_common_no_mat4
        COMMAND ${CMAKE_COMMAND}
        -DZANNA_SOURCE_DIR=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_SOURCE_DIR}/src/tests/tools/CheckGame3DCommonNoMat4.cmake)
set_tests_properties(g3d_game3d_common_no_mat4 PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS"
        LABELS "runtime;graphics3d;zia"
        TIMEOUT 10)

set(GAME3D_STARTER_DIR "${CMAKE_SOURCE_DIR}/examples/3d/game3d_starter")
set(GAME3D_STARTER_PROBE "${GAME3D_STARTER_DIR}/test.zia")
if (EXISTS "${GAME3D_STARTER_PROBE}")
    add_test(NAME g3d_game3d_starter_probe
            COMMAND $<TARGET_FILE:zanna> run "test.zia"
            WORKING_DIRECTORY "${GAME3D_STARTER_DIR}")
    set_tests_properties(g3d_game3d_starter_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 30)

    add_test(NAME g3d_game3d_starter_package_dry_run
            COMMAND $<TARGET_FILE:zanna> package "." --target tarball --dry-run
            WORKING_DIRECTORY "${GAME3D_STARTER_DIR}")
    # The starter embeds its model (self-contained examples/bin binary), so
    # the plan lists no loose asset mapping; the executable line proves the
    # dry run planned the embedded build.
    set_tests_properties(g3d_game3d_starter_package_dry_run PROPERTIES
            PASS_REGULAR_EXPRESSION "Executable: game3d_starter \\(build\\)"
            LABELS "runtime;graphics3d;zia"
            TIMEOUT 30)
endif ()

set(G3D_OVERHAUL_SHOWCASE_DIR "${CMAKE_SOURCE_DIR}/examples/3d/overhaul_showcase")
if (EXISTS "${G3D_OVERHAUL_SHOWCASE_DIR}/probe.zia")
    add_test(NAME g3d_overhaul_showcase_probe
            COMMAND $<TARGET_FILE:zanna> run "probe.zia"
            WORKING_DIRECTORY "${G3D_OVERHAUL_SHOWCASE_DIR}")
    set_tests_properties(g3d_overhaul_showcase_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 60)
endif ()

# Third-person action slice: the Game3D action-tier reference demo. The probe
# drives the shared Arena module deterministically (combat kill, lock-on,
# interactables, footsteps, day/night, save/load) with no window input.
set(G3D_ACTION_SLICE_DIR "${CMAKE_SOURCE_DIR}/examples/3d/action_slice")
if (EXISTS "${G3D_ACTION_SLICE_DIR}/test.zia")
    add_test(NAME g3d_action_slice_probe
            COMMAND $<TARGET_FILE:zanna> run "test.zia"
            WORKING_DIRECTORY "${G3D_ACTION_SLICE_DIR}")
    set_tests_properties(g3d_action_slice_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS: action_slice"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 120)
    add_test(NAME g3d_action_slice_package_dry_run
            COMMAND $<TARGET_FILE:zanna> package "." --target tarball --dry-run
            WORKING_DIRECTORY "${G3D_ACTION_SLICE_DIR}")
    set_tests_properties(g3d_action_slice_package_dry_run PROPERTIES
            PASS_REGULAR_EXPRESSION "action_slice"
            LABELS "runtime;graphics3d;zia"
            TIMEOUT 30)
endif ()

# GameBase3D/IScene3D scene-framework reference demo (menu -> fade -> play,
# 240 bounded frames). The one examples/3d demo that exercises the shared
# examples/games/lib scene stack end to end.
set(G3D_GAME3D_SCENES_DIR "${CMAKE_SOURCE_DIR}/examples/3d/game3d_scenes")
if (EXISTS "${G3D_GAME3D_SCENES_DIR}/main.zia")
    add_test(NAME g3d_game3d_scenes
            COMMAND $<TARGET_FILE:zanna> run "main.zia"
            WORKING_DIRECTORY "${G3D_GAME3D_SCENES_DIR}")
    set_tests_properties(g3d_game3d_scenes PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS: game3d_scenes"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software;ZANNA_DEMO_PROBE=1"
            TIMEOUT 60)
endif ()

set(GAME3D_OPENWORLD_SLICE_DIR "${CMAKE_SOURCE_DIR}/examples/3d/openworld_slice")
set(GAME3D_OPENWORLD_SLICE_PROBE "${GAME3D_OPENWORLD_SLICE_DIR}/test.zia")
if (APPLE)
    set(GAME3D_OPENWORLD_SLICE_GPU_BACKEND "metal")
elseif (WIN32)
    set(GAME3D_OPENWORLD_SLICE_GPU_BACKEND "d3d11")
else ()
    set(GAME3D_OPENWORLD_SLICE_GPU_BACKEND "opengl")
endif ()
if (EXISTS "${GAME3D_OPENWORLD_SLICE_PROBE}")
    add_test(NAME g3d_openworld_slice_probe
            COMMAND $<TARGET_FILE:zanna> run "test.zia"
            WORKING_DIRECTORY "${GAME3D_OPENWORLD_SLICE_DIR}")
    set_tests_properties(g3d_openworld_slice_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 45)

    add_test(NAME g3d_openworld_slice_perf_probe
            COMMAND $<TARGET_FILE:zanna> run "perf_probe.zia"
            WORKING_DIRECTORY "${GAME3D_OPENWORLD_SLICE_DIR}")
    set_tests_properties(g3d_openworld_slice_perf_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS: openworld_slice_perf_probe"
            LABELS "runtime;graphics3d;zia;requires_display;perf"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            RESOURCE_LOCK zanna_display
            TIMEOUT 120)

    # Budgets are order-of-magnitude guards over the ~29 ms avg / ~200 ms setup
    # measured on an AC-powered M-series host — generous enough for battery
    # E-cores, tight enough to catch algorithmic regressions.
    add_test(NAME g3d_openworld_slice_perf_harness
            COMMAND "${CMAKE_COMMAND}"
            -D "ZANNA_EXE=$<TARGET_FILE:zanna>"
            -D "WORKING_DIR=${GAME3D_OPENWORLD_SLICE_DIR}"
            -D "SCRIPT=perf_probe.zia"
            -D "BACKEND=software"
            -D "NAME=openworld_slice"
            -D "MAX_AVG_MS=250"
            -D "MAX_SETUP_MS=5000"
            -P "${CMAKE_SOURCE_DIR}/src/tests/tools/Graphics3DPerfHarness.cmake")
    set_tests_properties(g3d_openworld_slice_perf_harness PROPERTIES
            PASS_REGULAR_EXPRESSION "HARNESS: name=openworld_slice"
            LABELS "runtime;graphics3d;zia;requires_display;perf"
            RESOURCE_LOCK zanna_display
            TIMEOUT 150)

    add_test(NAME g3d_openworld_slice_streaming_hitch_probe
            COMMAND $<TARGET_FILE:zanna> run "streaming_hitch_probe.zia"
            WORKING_DIRECTORY "${GAME3D_OPENWORLD_SLICE_DIR}")
    set_tests_properties(g3d_openworld_slice_streaming_hitch_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS: openworld_slice_streaming_hitch_probe"
            LABELS "runtime;graphics3d;zia;perf"
            TIMEOUT 45)

    add_test(NAME g3d_openworld_slice_streaming_hitch_native_compressed_probe
            COMMAND $<TARGET_FILE:zanna> run "streaming_hitch_probe.zia"
            WORKING_DIRECTORY "${GAME3D_OPENWORLD_SLICE_DIR}")
    set_tests_properties(g3d_openworld_slice_streaming_hitch_native_compressed_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "native_compressed_upload=1.*native_raw_rgba_bytes=64.*native_compressed_bytes=16.*native_ram_reduction_pct=75.*native_vram_reduction_pct=75.*native_tolerance_checked=1"
            SKIP_REGULAR_EXPRESSION "SKIP: native compressed upload unsupported"
            LABELS "runtime;graphics3d;zia;requires_display;gpu_smoke;perf"
            ENVIRONMENT "ZANNA_3D_BACKEND=${GAME3D_OPENWORLD_SLICE_GPU_BACKEND};ZANNA_OPENWORLD_NATIVE_COMPRESSED_PROBE=1"
            TIMEOUT 45)

    add_test(NAME g3d_openworld_slice_long_traversal
            COMMAND $<TARGET_FILE:zanna> run "long_traversal.zia"
            WORKING_DIRECTORY "${GAME3D_OPENWORLD_SLICE_DIR}")
    set_tests_properties(g3d_openworld_slice_long_traversal PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS: openworld_slice_long_traversal"
            LABELS "runtime;graphics3d;zia;requires_display"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 60)

    add_test(NAME g3d_openworld_slice_visibility_dense_probe
            COMMAND $<TARGET_FILE:zanna> run "visibility_dense_probe.zia"
            WORKING_DIRECTORY "${GAME3D_OPENWORLD_SLICE_DIR}")
    set_tests_properties(g3d_openworld_slice_visibility_dense_probe PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS: openworld_slice_visibility_dense_probe"
            LABELS "runtime;graphics3d;zia;requires_display;perf"
            ENVIRONMENT "ZANNA_3D_BACKEND=software"
            TIMEOUT 45)

    add_test(NAME g3d_openworld_slice_package_dry_run
            COMMAND $<TARGET_FILE:zanna> package "." --target tarball --dry-run
            WORKING_DIRECTORY "${GAME3D_OPENWORLD_SLICE_DIR}")
    set_tests_properties(g3d_openworld_slice_package_dry_run PROPERTIES
            PASS_REGULAR_EXPRESSION "Asset: assets -> assets"
            LABELS "runtime;graphics3d;zia"
            TIMEOUT 30)
endif ()

set(GAME3D_OPENWORLD_SLICE_GPU_SMOKE "${GAME3D_OPENWORLD_SLICE_DIR}/gpu_smoke.zia")
if (EXISTS "${GAME3D_OPENWORLD_SLICE_GPU_SMOKE}")
    add_test(NAME g3d_openworld_slice_gpu_smoke
            COMMAND $<TARGET_FILE:zanna> run "gpu_smoke.zia"
            WORKING_DIRECTORY "${GAME3D_OPENWORLD_SLICE_DIR}")
    set_tests_properties(g3d_openworld_slice_gpu_smoke PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS:"
            SKIP_REGULAR_EXPRESSION "SKIP:"
            LABELS "runtime;graphics3d;zia;requires_display;gpu_smoke"
            ENVIRONMENT "ZANNA_3D_BACKEND=${GAME3D_OPENWORLD_SLICE_GPU_BACKEND}"
            TIMEOUT 45)
endif ()

if (NOT TARGET runtime_surface_audit_bundle)
    add_custom_target(runtime_surface_audit_bundle
            DEPENDS
            rtgen
            test_runtime_surface_audit
            test_runtime_name_map
            test_runtime_classes_catalog
            test_zia_static_calls
            test_zia_pointer_safety
            test_basic_runtime_calls
            test_rt_graphics_surface_link
            test_rt_audio_surface_link)
endif ()

# Keep the pull-request runtime lane explicit and reviewable. This list spans
# the C runtime contracts used most heavily by Studio, headless Studio service
# probes, and the VM/native boundary. Adding a test to the broad runtime or
# smoke labels cannot silently make this bounded cross-platform gate larger.
set(ZANNA_RUNTIME_SMOKE_CI_TESTS
        smoke_term_basic
        smoke_basic_oop
        test_rt_string
        test_rt_path
        test_rt_file_ext
        test_rt_watcher
        test_rt_regex
        test_rt_compiled_pattern
        test_rt_exec
        test_rt_gui_ide
        test_rt_zia_completion_stub
        zia_smoke_vec3_mutators
        zia_smoke_physics3d_fixed_step
        zia_zannastudio_source_freshness
        zia_zannastudio_safe_io
        zia_zannastudio_autosave_job
        zia_zannastudio_settings_migration
        zia_zannastudio_keybinding_settings
        zia_zannastudio_workspace_watcher
        zia_zannastudio_text_builders
        zia_zannastudio_edge_case_corpus
        zia_zannastudio_basic_query_job
        zia_zannastudio_shutdown_state)
foreach (_test IN LISTS ZANNA_RUNTIME_SMOKE_CI_TESTS)
    if (NOT TEST ${_test})
        message(FATAL_ERROR
                "Runtime smoke CI test '${_test}' is not registered")
    endif ()
    set_property(TEST ${_test} APPEND PROPERTY LABELS runtime-smoke-ci)
endforeach ()

if (ZANNA_BASH_EXECUTABLE AND ZANNA_BUILD_NATIVE_LINK_X86_64)
    set(ZANNA_RUNTIME_SMOKE_CI_NATIVE_TESTS
            native_smoke_zannastudio_completion_x64
            native_smoke_zannastudio_safe_io_x64
            native_smoke_zannastudio_basic_query_x64)
    foreach (_test IN LISTS ZANNA_RUNTIME_SMOKE_CI_NATIVE_TESTS)
        if (NOT TEST ${_test})
            message(FATAL_ERROR
                    "Native runtime smoke CI test '${_test}' is not registered")
        endif ()
        set_property(TEST ${_test} APPEND PROPERTY LABELS runtime-smoke-ci)
    endforeach ()
endif ()

# A display label is the single source of truth for access to the host windowing
# service. Keep every such test on the shared display lock so newly added probes
# cannot accidentally contend with the existing curated list. Windows removes
# this broad lock below and retains only its backend-specific D3D11 lock.
get_property(_zanna_all_tests DIRECTORY PROPERTY TESTS)
foreach (_test IN LISTS _zanna_all_tests)
    get_property(_labels TEST ${_test} PROPERTY LABELS)
    if ("requires_display" IN_LIST _labels)
        get_property(_resource_locks TEST ${_test} PROPERTY RESOURCE_LOCK)
        if (NOT "zanna_display" IN_LIST _resource_locks)
            set_property(TEST ${_test} APPEND PROPERTY RESOURCE_LOCK zanna_display)
        endif ()
    endif ()
endforeach ()
unset(_zanna_all_tests)

if (WIN32)
    # Keep the default Windows Debug lane focused on tests that complete in
    # under roughly 30 seconds. These measured VM/render/system simulations are
    # valuable coverage, but together add more than 20 minutes of serial CPU
    # time and create severe contention during an eight-worker full-suite run.
    set(_zanna_windows_slow_tests
            test_rt_game3d_thirdperson
            test_rt_game3d_combat
            test_rt_game3d_streaming_async
            test_rt_game3d_ragdoll_time
            test_rt_game3d_dialogue_facial
            test_rt_game3d_cinematics
            test_rt_game3d_hlod
            test_rt_restclient
            zia_zannastudio_scene_editor_2d
            zia_zannastudio_scene_editor_3d
            zia_zannastudio_console_search
            test_zia_repl)
    foreach (_test IN LISTS _zanna_windows_slow_tests)
        if (TEST ${_test})
            set_property(TEST ${_test} APPEND PROPERTY LABELS slow)
        endif ()
    endforeach ()
    unset(_zanna_windows_slow_tests)

    # The Windows build clears CTest's historical cost cache before every run.
    # Seed the known VM/render-heavy tests so the parallel scheduler starts them
    # while the short unit tests fill the remaining workers, rather than leaving
    # several minutes of single-test tail latency at the end of the suite.
    function(_zanna_set_windows_test_cost test_name cost)
        if (TEST ${test_name})
            set_property(TEST ${test_name} PROPERTY COST ${cost})
        endif ()
    endfunction()
    _zanna_set_windows_test_cost(test_zia_server_lsp 70)
    _zanna_set_windows_test_cost(zia_zannastudio_console_search 60)
    _zanna_set_windows_test_cost(test_zia_repl 40)
    _zanna_set_windows_test_cost(test_zia_bugfixes 30)
    _zanna_set_windows_test_cost(test_rt_scene3d 30)
    _zanna_set_windows_test_cost(zia_zannastudio_intellisense 15)

    # Every display-labelled test runs in its own process with window activation
    # disabled and windows hidden. A single Windows-wide display lock therefore
    # serialized unrelated software, 2D, and D3D probes without protecting shared
    # state. Keep only a narrow lock between tests that actually use D3D11.
    get_property(_zanna_windows_tests DIRECTORY PROPERTY TESTS)
    foreach (_test IN LISTS _zanna_windows_tests)
        get_property(_resource_locks TEST ${_test} PROPERTY RESOURCE_LOCK)
        if (_resource_locks)
            list(REMOVE_ITEM _resource_locks zanna_display)
            set_property(TEST ${_test} PROPERTY RESOURCE_LOCK "${_resource_locks}")
        endif ()

        get_property(_labels TEST ${_test} PROPERTY LABELS)
        get_property(_environment TEST ${_test} PROPERTY ENVIRONMENT)
        if ("d3d11" IN_LIST _labels OR
            "${_environment}" MATCHES "(^|;)ZANNA_3D_BACKEND=d3d11($|;)")
            set_property(TEST ${_test} APPEND PROPERTY RESOURCE_LOCK zanna_d3d11)
        endif ()
    endforeach ()
    unset(_zanna_windows_tests)
endif ()

# Fresh build trees have no historical CTest timing data. Give the two long,
# mutually exclusive lanes a small non-zero baseline so their chains begin
# immediately while zero-cost tests fill the remaining workers. Preserve
# explicit per-test costs above, and let CTest retain measured costs across
# subsequent canonical runs.
get_property(_zanna_scheduled_tests DIRECTORY PROPERTY TESTS)
foreach (_test IN LISTS _zanna_scheduled_tests)
    get_property(_cost_is_set TEST ${_test} PROPERTY COST SET)
    if (NOT _cost_is_set)
        get_property(_resource_locks TEST ${_test} PROPERTY RESOURCE_LOCK)
        if ("zanna_display" IN_LIST _resource_locks OR
            "zanna_d3d11" IN_LIST _resource_locks)
            set_property(TEST ${_test} PROPERTY COST 2)
        elseif ("aarch64_codegen_artifacts" IN_LIST _resource_locks)
            set_property(TEST ${_test} PROPERTY COST 1)
        endif ()
    endif ()
endforeach ()
unset(_zanna_scheduled_tests)
unset(_cost_is_set)
unset(_resource_locks)
unset(_test)

# Instrumentation can make otherwise short integration probes several times
# slower. Preserve the deliberately tight ordinary-build limits, but prevent an
# explicit per-test TIMEOUT from defeating the sanitizer driver's bounded
# timeout. Tests without an explicit limit continue to use ctest --timeout.
if (IL_SANITIZE_ADDRESS OR IL_SANITIZE_UNDEFINED OR IL_SANITIZE_THREAD)
    get_property(_zanna_instrumented_tests DIRECTORY PROPERTY TESTS)
    foreach (_test IN LISTS _zanna_instrumented_tests)
        get_property(_configured_timeout TEST ${_test} PROPERTY TIMEOUT)
        if ("${_configured_timeout}" MATCHES "^[1-9][0-9]*$")
            if (_configured_timeout LESS ZANNA_SANITIZER_TEST_TIMEOUT)
                set_property(TEST ${_test} PROPERTY TIMEOUT
                        ${ZANNA_SANITIZER_TEST_TIMEOUT})
            endif ()
        endif ()
    endforeach ()
    unset(_zanna_instrumented_tests)
endif ()

zanna_apply_display_test_environment()
zanna_apply_audio_test_environment()
