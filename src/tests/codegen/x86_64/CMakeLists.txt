# SPDX-License-Identifier: GPL-3.0-only
# File: tests/codegen/x86_64/CMakeLists.txt
# Purpose: Build the x86-64 codegen smoke tests that exercise the adapter
#          ILModule facade and verify minimal assembly output.

# Run pure x86-64 backend tests on every host.  Gate only tests that assemble,
# link, or execute x86-64 machine code through the native host path.
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" ZANNA_HOST_ARCH)
set(ZANNA_X86_64_NATIVE_TESTS OFF)
if (ZANNA_HOST_ARCH MATCHES "x86_64|amd64")
    set(ZANNA_X86_64_NATIVE_TESTS ON)
else ()
    message(STATUS "Building pure x86_64 codegen tests only on host arch: ${CMAKE_SYSTEM_PROCESSOR}")
endif ()

set(_zanna_codegen_x86_64_pure_tests
        abi_probe
        add_ret
        addr_stress
        addressing_modes
        asm_encoding
        call_indirect
        cf_stress
        determinism
        varargs_al
        ovf_trap
        udiv_urem
        i1_normalization
        pro_epi
        pipeline_equivalence
        f64_const
        return_values
        select
        select_cmov
        shifts
        string_literal
        regalloc_consistency
        regalloc_stress
        fp_stress
        udiv_pow2
        i32_checked_ovf
        null_trap
        object_only_cli)

set(_zanna_codegen_x86_64_native_tests
        diff_vm_native
        div_trap
        e2e_bitwise_cli
        e2e_fdiv_cli
        native_asm)

set(_zanna_codegen_x86_64_tests ${_zanna_codegen_x86_64_pure_tests})
if (ZANNA_X86_64_NATIVE_TESTS)
    list(APPEND _zanna_codegen_x86_64_tests ${_zanna_codegen_x86_64_native_tests})
endif ()

if (WIN32)
    list(REMOVE_ITEM _zanna_codegen_x86_64_tests varargs_al)
endif ()

foreach (_zanna_codegen_case IN LISTS _zanna_codegen_x86_64_tests)
    set(_zanna_codegen_target test_codegen_x86_64_${_zanna_codegen_case})
    set(_zanna_codegen_source
            ${ZANNA_TESTS_DIR}/codegen/x86_64/test_${_zanna_codegen_case}.cpp)

    add_executable(${_zanna_codegen_target} ${_zanna_codegen_source})
    if (WIN32)
        target_sources(${_zanna_codegen_target} PRIVATE
                ${ZANNA_TESTS_DIR}/common/WinDialogSuppress.c)
    endif ()
    target_link_libraries(${_zanna_codegen_target} PRIVATE zanna_testing)
    add_test(
            NAME codegen_x86_64_${_zanna_codegen_case}
            COMMAND ${_zanna_codegen_target})

    target_link_libraries(${_zanna_codegen_target} PRIVATE zanna_codegen_x86_64 zanna_test_common il_tools_common)

    if (_zanna_codegen_case STREQUAL "object_only_cli")
        target_compile_definitions(${_zanna_codegen_target}
                PRIVATE ZANNA_ILC_PATH="$<TARGET_FILE:zanna>")
    endif ()
    if (_zanna_codegen_case STREQUAL "div_trap")
        target_compile_definitions(${_zanna_codegen_target}
                PRIVATE ZANNA_ILC_PATH="$<TARGET_FILE:zanna>")
    endif ()
endforeach ()
