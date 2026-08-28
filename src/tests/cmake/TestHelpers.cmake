# SPDX-License-Identifier: GPL-3.0-only
# File: tests/cmake/TestHelpers.cmake
# Purpose: Helper functions to keep the test CMake files concise.

set(_ZANNA_TEST_LABEL_WHITELIST
        accessibility
        arm64
        assemble_link
        basic
        bytecode
        codegen
        comprehensive
        conformance
        contract
        differential
        e2e
        examples
        fuzz
        golden
        graphics3d
        il
        ilopt
        installer
        isolated
        namespace
        native_link
        native_run
        oop
        packaging
        perf
        requires_audio_disabled
        requires_display
        requires_graphics_disabled
        requires_ipv6
        requires_linux
        requires_linux_deb
        requires_linux_rpm
        requires_local_bind
        requires_macos
        requires_posix_shell
        requires_privileged_install
        requires_windows
        gui
        runtime
        slow
        smoke
        syntax_only
        tools
        tui
        unit
        vm
        windows_broken
        zia)

function(_zanna_normalize_label out_var label)
    string(TOLOWER "${label}" _label)

    if (_label STREQUAL "display_required")
        set(_label "requires_display")
    elseif (_label STREQUAL "basicstrings")
        set(_label "requires_posix_shell")
    elseif (_label STREQUAL "codegensyntaxonly")
        set(_label "syntax_only")
    elseif (_label STREQUAL "codegenassemblelink")
        set(_label "assemble_link")
    elseif (_label STREQUAL "nativex64run")
        set(_label "native_run")
    elseif (_label STREQUAL "nativelink")
        set(_label "native_link")
    endif ()

    set(${out_var} "${_label}" PARENT_SCOPE)
endfunction()

function(_zanna_validate_and_normalize_labels out_var)
    set(_normalized)
    foreach (_raw IN LISTS ARGN)
        if (_raw STREQUAL "")
            continue()
        endif ()

        string(REPLACE ";" ";" _split "${_raw}")
        foreach (_candidate IN LISTS _split)
            if (_candidate STREQUAL "")
                continue()
            endif ()

            _zanna_normalize_label(_label "${_candidate}")
            if (NOT _label IN_LIST _ZANNA_TEST_LABEL_WHITELIST)
                message(FATAL_ERROR
                        "Unknown test label '${_candidate}' (normalized '${_label}'). "
                        "Add it to _ZANNA_TEST_LABEL_WHITELIST in TestHelpers.cmake first.")
            endif ()
            list(APPEND _normalized "${_label}")
        endforeach ()
    endforeach ()

    list(REMOVE_DUPLICATES _normalized)
    set(${out_var} "${_normalized}" PARENT_SCOPE)
endfunction()

function(zanna_set_test_labels name)
    _zanna_validate_and_normalize_labels(_labels ${ARGN})
    list(JOIN _labels ";" _joined)
    set_tests_properties(${name} PROPERTIES LABELS "${_joined}")
    _zanna_apply_label_environment(${name} ${_labels})
endfunction()

function(zanna_add_test_labels name)
    _zanna_validate_and_normalize_labels(_labels ${ARGN})
    get_test_property(${name} LABELS _existing)
    if (_existing STREQUAL "NOTFOUND")
        set(_merged ${_labels})
    else ()
        set(_merged ${_existing} ${_labels})
    endif ()
    _zanna_validate_and_normalize_labels(_merged_norm ${_merged})
    list(JOIN _merged_norm ";" _joined)
    set_tests_properties(${name} PROPERTIES LABELS "${_joined}")
    _zanna_apply_label_environment(${name} ${_merged_norm})
endfunction()

function(_zanna_append_test_environment name entry)
    get_property(_has_env TEST ${name} PROPERTY ENVIRONMENT SET)
    if (_has_env)
        get_property(_env TEST ${name} PROPERTY ENVIRONMENT)
    else ()
        set(_env)
    endif ()
    if (NOT entry IN_LIST _env)
        list(APPEND _env "${entry}")
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "${_env}")
    endif ()
endfunction()

function(_zanna_apply_label_environment name)
    set(_labels ${ARGN})
    if ("requires_display" IN_LIST _labels OR "graphics3d" IN_LIST _labels)
        _zanna_append_test_environment(${name} "ZANNA_GFX_NO_ACTIVATE=1")
        _zanna_append_test_environment(${name} "ZANNA_GFX_HIDE_WINDOWS=1")
    endif ()
endfunction()

function(zanna_apply_display_test_environment)
    get_property(_tests DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY TESTS)
    foreach (_test IN LISTS _tests)
        get_property(_labels TEST ${_test} PROPERTY LABELS)
        if ("requires_display" IN_LIST _labels OR "graphics3d" IN_LIST _labels)
            _zanna_append_test_environment(${_test} "ZANNA_GFX_NO_ACTIVATE=1")
            _zanna_append_test_environment(${_test} "ZANNA_GFX_HIDE_WINDOWS=1")
        endif ()
    endforeach ()
endfunction()

function(zanna_apply_audio_test_environment)
    get_property(_tests DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY TESTS)
    foreach (_test IN LISTS _tests)
        _zanna_append_test_environment(${_test} "ZANNA_AUDIO_SILENT=1")
    endforeach ()
endfunction()

# Helper function to assign labels based on test name patterns.
function(_zanna_assign_test_label name)
    if (name MATCHES "^test_basic_" OR name MATCHES "^test_frontends_basic_" OR name MATCHES "^test_lowerer_" OR name MATCHES "^test_type_" OR name MATCHES "^test_builtin_" OR name MATCHES "^test_lowering_")
        zanna_set_test_labels(${name} basic)
    elseif (name MATCHES "^test_il_" OR name MATCHES "^il_" OR name MATCHES "^test_analysis_" OR name MATCHES "^test_irbuilder_")
        zanna_set_test_labels(${name} il)
    elseif (name MATCHES "^test_bytecode_" OR name MATCHES "^bytecode_" OR name MATCHES "^test_bytecode_vm$")
        zanna_set_test_labels(${name} vm bytecode)
    elseif (name MATCHES "^test_vm_" OR name MATCHES "^vm_")
        zanna_set_test_labels(${name} vm)
    elseif (name MATCHES "^test_shift_conformance$" OR name MATCHES "^test_subwidth_arith$" OR name MATCHES "^test_crosslayer_arith$" OR name MATCHES "^test_zia_arith_conformance$" OR name MATCHES "^basic_arith_")
        zanna_set_test_labels(${name} conformance)
    elseif (name MATCHES "^test_rt_" OR name MATCHES "^test_runtime_" OR name MATCHES "^runtime_" OR name MATCHES "^test_stringbuilder_" OR name MATCHES "^test_console_" OR name MATCHES "^test_convert_" OR name MATCHES "^test_catalog_")
        zanna_set_test_labels(${name} runtime)
    elseif (name MATCHES "^test_codegen_" OR name MATCHES "^test_emit_" OR name MATCHES "^test_target_" OR name MATCHES "^test_aarch64_" OR name MATCHES "^test_arm64_" OR name MATCHES "^codegen_" OR name MATCHES "^test_binenc_" OR name MATCHES "^test_linker_" OR name MATCHES "^test_objfile_" OR name MATCHES "^test_x86_")
        zanna_set_test_labels(${name} codegen)
    elseif (name MATCHES "^test_oop_" OR name MATCHES "^oop_" OR name MATCHES "^unit_basic_oop" OR name MATCHES "^test_method_" OR name MATCHES "^test_property_")
        zanna_set_test_labels(${name} oop)
    elseif (name MATCHES "^test_namespace_" OR name MATCHES "^test_using_" OR name MATCHES "^test_ns_")
        zanna_set_test_labels(${name} namespace)
    elseif (name MATCHES "^basic_error_" OR name MATCHES "^basic_ast_" OR name MATCHES "^basic_lex_" OR name MATCHES "^basic_to_il_" OR name MATCHES "^basic_semantics_" OR name MATCHES "^basic_proc_" OR name MATCHES "^basic_call_" OR name MATCHES "^basic_ns" OR name MATCHES "^basic_negatives_" OR name MATCHES "^basic_regress_" OR name MATCHES "^basic_select_" OR name MATCHES "^basic_boolean_" OR name MATCHES "^basic_gosub_" OR name MATCHES "^basic_globals_" OR name MATCHES "^basic_print_" OR name MATCHES "^basic_namespace_" OR name MATCHES "^basic_trycatch_" OR name MATCHES "^basic_fileio_" OR name MATCHES "^basic_zanna_" OR name MATCHES "^basic_int_" OR name MATCHES "^basic_shared_" OR name MATCHES "^basic_not_" OR name MATCHES "^basic_runtime_ns_" OR name MATCHES "^basic_using_" OR name MATCHES "^basic_arrays_" OR name MATCHES "^example_basic_" OR name MATCHES "^errors_map_" OR name MATCHES "^eh_runtime_" OR name MATCHES "^runtime_classes_" OR name MATCHES "^oop_runtime_" OR name MATCHES "^numerics_il_" OR name MATCHES "^il_quickstart_" OR name MATCHES "^il_zanna_ns_" OR name MATCHES "^il_legacy_" OR name MATCHES "^basic_select_case_")
        zanna_set_test_labels(${name} golden)
    elseif (name MATCHES "^basic_oop_" OR name MATCHES "^basic_numerics_" OR name MATCHES "^basic_args_" OR name MATCHES "^basic_bug" OR name MATCHES "^basic_array" OR name MATCHES "^basic_do_" OR name MATCHES "^basic_for_" OR name MATCHES "^basic_const_" OR name MATCHES "^basic_math_" OR name MATCHES "^basic_abs_" OR name MATCHES "^basic_factorial$" OR name MATCHES "^basic_fibonacci$" OR name MATCHES "^basic_random_" OR name MATCHES "^front_basic_" OR name MATCHES "^monte_carlo_" OR name MATCHES "^random_walk$" OR name MATCHES "^mem2reg_" OR name MATCHES "^ilc_mem2reg_")
        zanna_set_test_labels(${name} e2e)
    elseif (name MATCHES "^il_opt_" OR name MATCHES "^constfold_" OR name MATCHES "^simplifycfg_")
        zanna_set_test_labels(${name} ilopt)
    elseif (name MATCHES "^smoke_" OR name MATCHES "^basic_sum_" OR name MATCHES "^basic_repros$")
        zanna_set_test_labels(${name} smoke)
    elseif (name MATCHES "^tui_")
        zanna_set_test_labels(${name} tui)
    elseif (name MATCHES "^perf_")
        zanna_set_test_labels(${name} perf)
    elseif (name MATCHES "^example_smoke_" OR name MATCHES "^examples_")
        zanna_set_test_labels(${name} examples)
    elseif (name MATCHES "^fuzz_" OR name MATCHES "^test_fuzz_")
        zanna_set_test_labels(${name} fuzz)
    elseif (name MATCHES "^installer_")
        zanna_set_test_labels(${name} installer)
    elseif (name MATCHES "^test_cli_" OR name MATCHES "^NoAssertFalseGuard$" OR name MATCHES "^test_tools_" OR name MATCHES "^test_zbasic_" OR name MATCHES "^test_zia_server_")
        zanna_set_test_labels(${name} tools)
    elseif (name MATCHES "^test_support$" OR name MATCHES "^test_run_" OR name MATCHES "^test_expected_" OR name MATCHES "^test_ident_" OR name MATCHES "^test_path_" OR name MATCHES "^test_integer_" OR name MATCHES "^test_window$" OR name MATCHES "^test_pixels$" OR name MATCHES "^test_drawing$" OR name MATCHES "^test_input$")
        zanna_set_test_labels(${name} unit)
    endif ()
endfunction()

function(zanna_add_test target)
    set(options NO_CTEST)
    set(oneValueArgs TEST_NAME)
    set(multiValueArgs SRCS LIBS COMPILE_DEFS INCLUDES)
    cmake_parse_arguments(VT "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    if (VT_SRCS)
        set(_zanna_sources ${VT_SRCS})
    else ()
        set(_zanna_sources ${VT_UNPARSED_ARGUMENTS})
    endif ()
    if (NOT _zanna_sources)
        message(FATAL_ERROR "zanna_add_test requires at least one source")
    endif ()
    add_executable(${target} ${_zanna_sources})
    set_target_properties(${target} PROPERTIES
            C_STANDARD 11
            C_STANDARD_REQUIRED ON
            C_EXTENSIONS OFF)
    if (WIN32)
        target_sources(${target} PRIVATE
                ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../common/WinDialogSuppress.c)
    else ()
        target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200809L)
        if (NOT APPLE)
            target_compile_definitions(${target} PRIVATE _DEFAULT_SOURCE)
        endif ()
    endif ()
    target_link_libraries(${target} PRIVATE zanna_testing)
    if (VT_LIBS)
        target_link_libraries(${target} PRIVATE ${VT_LIBS})
    endif ()
    if (VT_COMPILE_DEFS)
        target_compile_definitions(${target} PRIVATE ${VT_COMPILE_DEFS})
    endif ()
    if (VT_INCLUDES)
        target_include_directories(${target} PRIVATE ${VT_INCLUDES})
    endif ()
    if (VT_NO_CTEST)
        return()
    endif ()
    if (VT_TEST_NAME)
        set(_zanna_test_name ${VT_TEST_NAME})
    else ()
        set(_zanna_test_name ${target})
    endif ()
    add_test(NAME ${_zanna_test_name} COMMAND ${target})
    set_tests_properties(${_zanna_test_name} PROPERTIES SKIP_RETURN_CODE 77)
    _zanna_assign_test_label(${_zanna_test_name})
endfunction()

function(zanna_add_ctest name)
    if (ARGC EQUAL 2 AND "${name}" STREQUAL "${ARGV1}")
        return()
    endif ()
    add_test(NAME ${name} COMMAND ${ARGN})
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
    _zanna_assign_test_label(${name})
endfunction()

# Ensure no new golden directories sneak into the tree. Golden tests must rely on
# the shared suites under tests/golden/ rather than ad-hoc "goldens" folders.
function(zanna_assert_no_goldens_directories)
    file(
            GLOB_RECURSE _zanna_goldens
            LIST_DIRECTORIES true
            RELATIVE "${CMAKE_SOURCE_DIR}/src/tests"
            "${CMAKE_SOURCE_DIR}/src/tests/*")

    list(FILTER _zanna_goldens INCLUDE REGEX "/goldens$")

    if (_zanna_goldens)
        list(TRANSFORM _zanna_goldens PREPEND "${CMAKE_SOURCE_DIR}/src/tests/")
        list(JOIN _zanna_goldens "\n  " _zanna_goldens_pretty)
        message(
                FATAL_ERROR
                "Golden directories named 'goldens' are prohibited. Remove or rename:\n  ${_zanna_goldens_pretty}")
    endif ()
endfunction()

# =============================================================================
# Golden Test Helpers
# =============================================================================
# These functions reduce golden test registration from 6-8 lines to 1 line.
# Each wraps one of the 10 runner cmake scripts.

# _golden_error: BASIC compile error test (check_error.cmake)
# Reads .stderr file, compiles .bas, verifies stderr matches.
# Optional: EXPECT_EXIT_ZERO for tests that should compile without error.
function(_golden_error name dir bas_stem)
    set(options EXPECT_EXIT_ZERO)
    cmake_parse_arguments(GE "${options}" "" "" ${ARGN})
    set(_bas ${dir}/${bas_stem}.bas)
    set(_stderr ${dir}/${bas_stem}.stderr)
    file(READ ${_stderr} _expect)
    string(STRIP "${_expect}" _expect)
    set(_extra_args "")
    if (GE_EXPECT_EXIT_ZERO)
        set(_extra_args -DEXPECT_EXIT_ZERO=TRUE)
    endif ()
    zanna_add_ctest(${name}
            ${CMAKE_COMMAND}
            -DILC=${BASIC_ILC}
            -DBAS_FILE=${_bas}
            "-DEXPECT=${_expect}"
            ${_extra_args}
            -P ${_ZANNA_GOLDEN_DIR}/basic_errors/check_error.cmake)
endfunction()

# _golden_basic_run: BASIC run + stdout comparison (check_basic_run_output.cmake)
function(_golden_basic_run name dir bas_stem stdout_file)
    set(options "")
    set(oneValueArgs CLEANUP_FILE)
    cmake_parse_arguments(GR "${options}" "${oneValueArgs}" "" ${ARGN})
    file(READ ${dir}/${stdout_file} _expect)
    string(STRIP "${_expect}" _expect)
    set(_extra_args "")
    if (GR_CLEANUP_FILE)
        set(_extra_args "-DCLEANUP_FILE=${GR_CLEANUP_FILE}")
    endif ()
    zanna_add_ctest(${name}
            ${CMAKE_COMMAND}
            -DILC=${BASIC_ILC}
            -DBAS_FILE=${dir}/${bas_stem}.bas
            "-DEXPECT=${_expect}"
            ${_extra_args}
            -P ${_ZANNA_GOLDEN_DIR}/arrays/check_basic_run_output.cmake)
endfunction()

# _golden_basic_run_error: BASIC run + stderr comparison (check_basic_run_error.cmake)
function(_golden_basic_run_error name dir bas_stem stderr_file)
    file(READ ${dir}/${stderr_file} _expect)
    string(STRIP "${_expect}" _expect)
    zanna_add_ctest(${name}
            ${CMAKE_COMMAND}
            -DILC=${BASIC_ILC}
            -DBAS_FILE=${dir}/${bas_stem}.bas
            "-DEXPECT=${_expect}"
            -P ${_ZANNA_GOLDEN_DIR}/basic_errors/check_basic_run_error.cmake)
endfunction()

# _golden_il_run: IL run + stdout comparison (check_run_output.cmake)
function(_golden_il_run name il_file expect_str)
    zanna_add_ctest(${name}
            ${CMAKE_COMMAND}
            -DILC=${BASIC_ILC}
            -DIL_FILE=${il_file}
            "-DEXPECT=${expect_str}"
            -P ${_ZANNA_GOLDEN_DIR}/check_run_output.cmake)
endfunction()

# _golden_il_run_file: IL run + stdout from file (check_run_output.cmake)
function(_golden_il_run_file name il_file expect_file)
    file(READ ${expect_file} _expect)
    string(STRIP "${_expect}" _expect)
    zanna_add_ctest(${name}
            ${CMAKE_COMMAND}
            -DILC=${BASIC_ILC}
            -DIL_FILE=${il_file}
            "-DEXPECT=${_expect}"
            -P ${_ZANNA_GOLDEN_DIR}/check_run_output.cmake)
endfunction()

# _golden_il_opt: IL optimizer golden test (check_opt.cmake)
function(_golden_il_opt name il_in_file golden_file)
    set(oneValueArgs PASSES)
    cmake_parse_arguments(GO "" "${oneValueArgs}" "" ${ARGN})
    set(_extra_args "")
    if (GO_PASSES)
        set(_extra_args -DPASSES=${GO_PASSES})
    endif ()
    zanna_add_ctest(${name}
            ${CMAKE_COMMAND}
            -DILC=${BASIC_ILC}
            -DIL_FILE=${il_in_file}
            -DGOLDEN=${golden_file}
            ${_extra_args}
            -P ${_ZANNA_GOLDEN_DIR}/il_opt/check_opt.cmake)
endfunction()

# _golden_constfold: IL constant folding golden test (check_constfold.cmake)
function(_golden_constfold name il_file golden_file)
    zanna_add_ctest(${name}
            ${CMAKE_COMMAND}
            -DILC=${BASIC_ILC}
            -DIL_FILE=${il_file}
            -DGOLDEN=${golden_file}
            -P ${_ZANNA_GOLDEN_DIR}/constfold/check_constfold.cmake)
endfunction()

# _golden_basic_to_il: BASIC→IL golden test (check_il.cmake)
function(_golden_basic_to_il name bas_file golden_il_file)
    zanna_add_ctest(${name}
            ${CMAKE_COMMAND}
            -DILC=${BASIC_ILC}
            -DBAS_FILE=${bas_file}
            -DGOLDEN=${golden_il_file}
            -P ${_ZANNA_GOLDEN_DIR}/basic_to_il/check_il.cmake)
endfunction()

# _golden_vm_trap_loc: IL run expecting trap at specific location (check_vm_trap_loc.cmake)
function(_golden_vm_trap_loc name il_file expect_str)
    zanna_add_ctest(${name}
            ${CMAKE_COMMAND}
            -DILC=${BASIC_ILC}
            -DIL_FILE=${il_file}
            "-DEXPECT=${expect_str}"
            -P ${_ZANNA_GOLDEN_DIR}/check_vm_trap_loc.cmake)
endfunction()

# _golden_exit_code: Run program and check exit code (check_run_exit_code.cmake)
function(_golden_exit_code name)
    set(oneValueArgs BAS_FILE IL_FILE EXIT_CODE EXPECT)
    cmake_parse_arguments(GX "" "${oneValueArgs}" "" ${ARGN})
    set(_args "")
    if (GX_BAS_FILE)
        list(APPEND _args -DBAS_FILE=${GX_BAS_FILE})
    endif ()
    if (GX_IL_FILE)
        list(APPEND _args -DIL_FILE=${GX_IL_FILE})
    endif ()
    if (GX_EXIT_CODE)
        list(APPEND _args -DEXPECT_EXIT=${GX_EXIT_CODE})
    endif ()
    if (GX_EXPECT)
        list(APPEND _args "-DEXPECT=${GX_EXPECT}")
    endif ()
    zanna_add_ctest(${name}
            ${CMAKE_COMMAND}
            -DILC=${BASIC_ILC}
            ${_args}
            -P ${_ZANNA_GOLDEN_DIR}/check_run_exit_code.cmake)
endfunction()

zanna_assert_no_goldens_directories()
