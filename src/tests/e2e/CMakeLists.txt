# SPDX-License-Identifier: GPL-3.0-only
# File: tests/e2e/CMakeLists.txt
# Purpose: End-to-end CLI and VM integration tests.

function(zanna_add_e2e_vm_examples_smoke)
    if (TEST vm_examples)
        return()
    endif ()

    set(_ZANNA_E2E_DIR ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
    zanna_add_ctest(vm_examples
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DSRC_DIR=${CMAKE_SOURCE_DIR}
            -P ${_ZANNA_E2E_DIR}/test_vm_examples.cmake)
    zanna_set_test_labels(vm_examples vm)
endfunction()

function(zanna_add_e2e_vm_suite)
    if (TEST vm_strings_example)
        return()
    endif ()

    set(_ZANNA_E2E_DIR ${CMAKE_CURRENT_FUNCTION_LIST_DIR})

    if (NOT TARGET float_out)
        zanna_add_test(float_out ${_ZANNA_E2E_DIR}/support/FloatOut.cpp NO_CTEST)
    endif ()

    zanna_add_ctest(vm_strings_example
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DSRC_DIR=${CMAKE_SOURCE_DIR}
            -P ${_ZANNA_E2E_DIR}/test_vm_strings.cmake)
    zanna_set_test_labels(vm_strings_example vm)

    zanna_add_ctest(vm_step_limit
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DSRC_DIR=${CMAKE_SOURCE_DIR}
            -P ${_ZANNA_E2E_DIR}/test_vm_step_limit.cmake)
    zanna_set_test_labels(vm_step_limit vm)

    zanna_add_ctest(vm_block_params
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DSRC_DIR=${CMAKE_SOURCE_DIR}
            -P ${_ZANNA_E2E_DIR}/test_vm_block_params.cmake)
    zanna_set_test_labels(vm_block_params vm)

    zanna_add_ctest(vm_parallel_callbacks
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DSRC_DIR=${CMAKE_SOURCE_DIR}
            -P ${_ZANNA_E2E_DIR}/test_vm_parallel_callbacks.cmake)
    zanna_set_test_labels(vm_parallel_callbacks vm)

    zanna_add_ctest(vm_math_core
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DSRC_DIR=${CMAKE_SOURCE_DIR}
            -P ${_ZANNA_E2E_DIR}/test_vm_math_core.cmake)
    zanna_set_test_labels(vm_math_core vm)

    zanna_add_ctest(vm_math_trigpow
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DSRC_DIR=${CMAKE_SOURCE_DIR}
            -P ${_ZANNA_E2E_DIR}/test_vm_math_trigpow.cmake)
    zanna_set_test_labels(vm_math_trigpow vm)

    zanna_add_ctest(vm_rec_fact
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DSRC_DIR=${CMAKE_SOURCE_DIR}
            -P ${_ZANNA_E2E_DIR}/test_vm_rec_fact.cmake)
    zanna_set_test_labels(vm_rec_fact vm)

    zanna_add_ctest(vm_trace_src
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/examples/vbasic/trace_src.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/TraceSrcTests.bas.out
            -P ${_ZANNA_E2E_DIR}/test_trace_src.cmake)
    set_tests_properties(vm_trace_src PROPERTIES RESOURCE_LOCK vm_trace_artifacts)
    zanna_set_test_labels(vm_trace_src vm)

    zanna_add_ctest(vm_break_src_exact
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DSRC_FILE=src/tests/e2e/BreakSrcExact.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/golden/break_src_exact.out
            -DLINE=1
            -DROOT=${CMAKE_SOURCE_DIR}
            -P ${_ZANNA_E2E_DIR}/test_break_src_exact.cmake)
    set_tests_properties(vm_break_src_exact PROPERTIES RESOURCE_LOCK vm_trace_artifacts)
    zanna_set_test_labels(vm_break_src_exact vm)

    zanna_add_ctest(vm_break_src_coalesce
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DSRC_FILE=src/tests/e2e/BreakSrcCoalesce.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/golden/break_src_coalesce.out
            -DLINE=1
            -DROOT=${CMAKE_SOURCE_DIR}
            -P ${_ZANNA_E2E_DIR}/test_break_src_exact.cmake)
    set_tests_properties(vm_break_src_coalesce PROPERTIES RESOURCE_LOCK vm_trace_artifacts)
    zanna_set_test_labels(vm_break_src_coalesce vm)

    zanna_add_ctest(front_basic_example
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DSRC_DIR=${CMAKE_SOURCE_DIR}
            -P ${_ZANNA_E2E_DIR}/test_front_basic.cmake)

    zanna_add_ctest(basic_math_phase1
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic_math_phase1.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic_math_phase1.bas.out
            -P ${_ZANNA_E2E_DIR}/test_basic_math_phase1.cmake)

    zanna_add_ctest(basic_math_phase2
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic_math_phase2.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic_math_phase2.bas.out
            -P ${_ZANNA_E2E_DIR}/test_basic_math_phase1.cmake)

    zanna_add_ctest(basic_abs_mixed
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic_abs_mixed.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic_abs_mixed.bas.out
            -P ${_ZANNA_E2E_DIR}/test_basic_math_phase1.cmake)

    zanna_add_ctest(basic_numerics_addmul
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic/numerics_addmul.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic/numerics_addmul.bas.out
            -DEXPECT_SUBSTR=Overflow
            -P ${_ZANNA_E2E_DIR}/test_basic_numeric_overflow.cmake)
    zanna_set_test_labels(basic_numerics_addmul vm)

    zanna_add_ctest(basic_numerics_pow
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic/numerics_pow.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic/numerics_pow.bas.out
            -DEXPECT_SUBSTR=domain\ error
            -P ${_ZANNA_E2E_DIR}/test_basic_numeric_overflow.cmake)
    zanna_set_test_labels(basic_numerics_pow vm)

    zanna_add_ctest(basic_numerics_conversions
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic/numerics_conversions.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic/numerics_conversions.bas.out
            -DEXPECT_SUBSTR=InvalidCast
            -P ${_ZANNA_E2E_DIR}/test_basic_numeric_overflow.cmake)
    zanna_set_test_labels(basic_numerics_conversions vm)

    zanna_add_ctest(basic_numerics_val
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic/numerics_val.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic/numerics_val.bas.out
            -P ${_ZANNA_E2E_DIR}/test_basic_math_phase1.cmake)
    zanna_set_test_labels(basic_numerics_val vm)

    zanna_add_ctest(basic_numerics_literal_suffixes
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic/numerics_literal_suffixes.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic/numerics_literal_suffixes.bas.out
            -P ${_ZANNA_E2E_DIR}/test_basic_math_phase1.cmake)
    zanna_set_test_labels(basic_numerics_literal_suffixes vm)

    zanna_add_ctest(basic_select_case_run
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic/select_case_run.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic/select_case_run.bas.out
            -P ${_ZANNA_E2E_DIR}/test_basic_math_phase1.cmake)
    zanna_set_test_labels(basic_select_case_run vm)

    zanna_add_ctest(basic_array_sum
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic_array_sum.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic_array_sum.bas.out
            -P ${_ZANNA_E2E_DIR}/test_basic_array_sum.cmake)
    zanna_set_test_labels(basic_array_sum vm)

    zanna_add_ctest(basic_do_exit
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/do_exit.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/do_exit.bas.out
            -P ${_ZANNA_E2E_DIR}/test_do_exit.cmake)
    zanna_set_test_labels(basic_do_exit vm)

    zanna_add_ctest(basic_for_step_neg
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/for_step_neg.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/for_step_neg.bas.out
            -P ${_ZANNA_E2E_DIR}/test_do_exit.cmake)
    zanna_set_test_labels(basic_for_step_neg vm)

    zanna_add_ctest(basic_oop_field_access
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic_oop_field.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic_oop_field.bas.out
            -P ${_ZANNA_E2E_DIR}/test_basic_math_phase1.cmake)
    zanna_set_test_labels(basic_oop_field_access vm)

    zanna_add_ctest(basic_oop_string_property
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/test_oop_string_property.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/test_oop_string_property.bas.out
            -P ${_ZANNA_E2E_DIR}/test_basic_math_phase1.cmake)
    zanna_set_test_labels(basic_oop_string_property vm)

    zanna_add_ctest(basic_print_field_string
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/test_print_field_string.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/test_print_field_string.bas.out
            -P ${_ZANNA_E2E_DIR}/test_basic_math_phase1.cmake)
    zanna_set_test_labels(basic_print_field_string vm)

    # Arrays as class fields must allocate and be accessible
    zanna_add_ctest(basic_oop_array_field
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic_oop_array_field.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic_oop_array_field.bas.out
            -P ${_ZANNA_E2E_DIR}/test_basic_math_phase1.cmake)
    zanna_set_test_labels(basic_oop_array_field vm)


    zanna_add_ctest(basic_const_string
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/test_const_string.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/test_const_string.bas.out
            -P ${_ZANNA_E2E_DIR}/test_basic_math_phase1.cmake)
    zanna_set_test_labels(basic_const_string vm)

    zanna_add_ctest(basic_oop_function_return
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/test_oop_function_return.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/test_oop_function_return.bas.out
            -P ${_ZANNA_E2E_DIR}/test_basic_math_phase1.cmake)
    zanna_set_test_labels(basic_oop_function_return vm)

    zanna_add_ctest(basic_oop_multi_obj_return
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/test_oop_multi_obj_return.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/test_oop_multi_obj_return.bas.out
            -P ${_ZANNA_E2E_DIR}/test_basic_math_phase1.cmake)
    zanna_set_test_labels(basic_oop_multi_obj_return vm)

    # Command-line arguments: no args
    zanna_add_ctest(basic_args_none
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic_args_none.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic_args_none.bas.out
            -P ${_ZANNA_E2E_DIR}/test_basic_args_run.cmake)
    zanna_set_test_labels(basic_args_none vm)

    zanna_add_ctest(basic_args_none_debug
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic_args_none.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic_args_none.bas.out
            -DDEBUG_VM=ON
            -P ${_ZANNA_E2E_DIR}/test_basic_args_run.cmake)
    zanna_set_test_labels(basic_args_none_debug vm)

    # Command-line arguments: some args
    # Pass two program arguments: "foo" and "bar baz". Semicolons are CMake's
    # list separators, so escape it (\;) to keep both elements inside the single
    # -DARGS=... argument. Spaces do not need escaping here.
    zanna_add_ctest(basic_args_some
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic_args_some.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic_args_some.bas.out
            "-DARGS=foo\;bar baz"
            -P ${_ZANNA_E2E_DIR}/test_basic_args_run.cmake)
    zanna_set_test_labels(basic_args_some vm)

    # BUG-053: Access module-level array inside FUNCTION
    zanna_add_ctest(basic_bug053_global_array_in_proc
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/bug053_global_array_in_proc.bas
            -P ${_ZANNA_E2E_DIR}/test_basic_compile_only.cmake)
    zanna_set_test_labels(basic_bug053_global_array_in_proc vm)

    # BUG-052: ON ERROR GOTO no-op should not produce empty block
    zanna_add_ctest(basic_bug052_on_error_empty_block
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/bug052_on_error_empty_block.bas
            -P ${_ZANNA_E2E_DIR}/test_basic_compile_only.cmake)
    zanna_set_test_labels(basic_bug052_on_error_empty_block vm)

    zanna_add_ctest(basic_oop_local_string
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/test_oop_local_string.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/test_oop_local_string.bas.out
            -P ${_ZANNA_E2E_DIR}/test_basic_math_phase1.cmake)
    zanna_set_test_labels(basic_oop_local_string vm)

    zanna_add_ctest(basic_oop_method_return
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic_oop_method_return.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic_oop_method_return.bas.out
            -P ${_ZANNA_E2E_DIR}/test_basic_math_phase1.cmake)
    zanna_set_test_labels(basic_oop_method_return vm)

    zanna_add_ctest(basic_gosub_return
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/gosub_return.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/gosub_return.bas.out
            -P ${_ZANNA_E2E_DIR}/test_basic_math_phase1.cmake)
    zanna_set_test_labels(basic_gosub_return vm)

    zanna_add_ctest(basic_array_oob
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic_array_oob.bas
            "-DEXPECT_SUBSTR=rt_arr_i32: index 4 out of bounds (len=4)"
            -P ${_ZANNA_E2E_DIR}/test_basic_array_oob.cmake)
    zanna_set_test_labels(basic_array_oob vm)

    zanna_add_ctest(basic_factorial
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/factorial.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/golden/e2e/factorial.out
            -P ${_ZANNA_E2E_DIR}/test_basic_math_phase1.cmake)

    zanna_add_ctest(basic_fibonacci
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/fibonacci.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/golden/e2e/fibonacci.out
            -P ${_ZANNA_E2E_DIR}/test_basic_math_phase1.cmake)

    zanna_add_ctest(basic_random_repro
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DSRC_DIR=${CMAKE_SOURCE_DIR}
            -P ${_ZANNA_E2E_DIR}/test_basic_random_repro.cmake)

    zanna_add_ctest(basic_numerics_all
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic/numerics_all.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/basic/numerics_all.bas.out
            -P ${_ZANNA_E2E_DIR}/test_basic_math_phase1.cmake)

    zanna_add_ctest(monte_carlo_pi
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DFLOAT_OUT=$<TARGET_FILE:float_out>
            -DSRC_DIR=${CMAKE_SOURCE_DIR}
            -P ${_ZANNA_E2E_DIR}/test_monte_carlo_pi.cmake)

    zanna_add_ctest(random_walk
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DSRC_DIR=${CMAKE_SOURCE_DIR}
            -P ${_ZANNA_E2E_DIR}/test_random_walk.cmake)

    zanna_add_ctest(il_opt_equiv
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DIL_FILE=${CMAKE_SOURCE_DIR}/src/tests/golden/il_opt/e2e.in.il
            -P ${_ZANNA_E2E_DIR}/test_il_opt_equiv.cmake)

    zanna_add_ctest(simplifycfg_smoke
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DIL_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/simplifycfg_smoke.il
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/simplifycfg_smoke.out
            -P ${_ZANNA_E2E_DIR}/test_simplifycfg_smoke.cmake)
    zanna_set_test_labels(simplifycfg_smoke ilopt smoke)

    zanna_add_ctest(constfold_equiv
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DIL_FILE=${CMAKE_SOURCE_DIR}/src/tests/il/e2e/math_constfold.il
            -P ${_ZANNA_E2E_DIR}/test_constfold_equiv.cmake)

    zanna_add_ctest(mem2reg_basic
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/data/mem2reg.bas
            -P ${_ZANNA_E2E_DIR}/test_mem2reg_basic.cmake)

    zanna_add_ctest(ilc_mem2reg_stats
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DIL_FILE=${CMAKE_SOURCE_DIR}/src/tests/golden/il_opt/mem2reg_simple.in.il
            -P ${_ZANNA_E2E_DIR}/test_mem2reg_stats.cmake)

    zanna_add_ctest(ilc_mem2reg_stats_multi
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DIL_FILE=${CMAKE_SOURCE_DIR}/src/tests/golden/il_opt/mem2reg_multi.in.il
            -P ${_ZANNA_E2E_DIR}/test_mem2reg_stats_multi.cmake)

    # TRY/FINALLY support tests (requires standard VM for runtime exception handling)
    zanna_add_ctest(e2e_try_finally
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/test_try_finally.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/test_try_finally.bas.out
            -DDEBUG_VM=1
            -P ${_ZANNA_E2E_DIR}/test_basic_math_phase1.cmake)
    zanna_set_test_labels(e2e_try_finally e2e vm)

    # Constructor arguments tests (multiple types: INTEGER, DOUBLE, STRING)
    zanna_add_ctest(e2e_ctor_args
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/test_ctor_args.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/test_ctor_args.bas.out
            -P ${_ZANNA_E2E_DIR}/test_basic_math_phase1.cmake)
    zanna_set_test_labels(e2e_ctor_args e2e vm)

    # Graphics API basic tests (no window required)
    zanna_add_ctest(e2e_graphics_basic
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/test_graphics_basic.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/test_graphics_basic.bas.out
            -P ${_ZANNA_E2E_DIR}/test_basic_math_phase1.cmake)
    zanna_set_test_labels(e2e_graphics_basic e2e vm)

    # Graphics window tests (requires display - skip in headless CI)
    zanna_add_ctest(e2e_graphics_window
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/test_graphics_window.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/test_graphics_window.bas.out
            -P ${_ZANNA_E2E_DIR}/test_basic_math_phase1.cmake)
    zanna_set_test_labels(e2e_graphics_window e2e vm requires_display)

    # DateTime API tests
    zanna_add_ctest(e2e_datetime
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/test_datetime.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/test_datetime.bas.out
            -P ${_ZANNA_E2E_DIR}/test_basic_math_phase1.cmake)
    zanna_set_test_labels(e2e_datetime e2e)

    # Map API tests
    zanna_add_ctest(e2e_map
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/test_map.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/test_map.bas.out
            -P ${_ZANNA_E2E_DIR}/test_basic_math_phase1.cmake)
    zanna_set_test_labels(e2e_map e2e)

    # USING statement tests (resource cleanup)
    # Skip on Windows: USING statement with DESTROY causes crash (to be investigated)
    if (NOT WIN32)
        zanna_add_ctest(e2e_using
                ${CMAKE_COMMAND}
                -DILC=$<TARGET_FILE:zanna>
                -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/test_using.bas
                -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/test_using.bas.out
                -P ${_ZANNA_E2E_DIR}/test_basic_math_phase1.cmake)
        zanna_set_test_labels(e2e_using e2e)
    endif ()
endfunction()

# OOP runtime e2e: List + String + Object (avoids reserved keyword collisions)
zanna_add_ctest(oop_collections_string_report
        ${CMAKE_COMMAND}
        -DILC=$<TARGET_FILE:zanna>
        -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/oop/collections_string_report.bas
        -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/oop/collections_string_report.bas.out
        -P ${CMAKE_SOURCE_DIR}/src/tests/e2e/test_basic_math_phase1.cmake)
zanna_set_test_labels(oop_collections_string_report e2e vm oop)

function(zanna_add_e2e_codegen_tests)
    if (DEFINED ZANNA_E2E_CODEGEN_DONE)
        return()
    endif ()

    set(_ZANNA_E2E_DIR ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
    set(CODEGEN_E2E ${_ZANNA_E2E_DIR}/test_codegen.cmake)

    if (IL_ENABLE_X64_ASM_SYNTAX_CHECK AND CLANG_FOUND)
        zanna_add_ctest(codegen_syntax_only
                ${CMAKE_COMMAND}
                "-DCLANG_BIN=${CLANG_EXECUTABLE}"
                "-DASM_FLAGS=${IL_X64_ASM_FLAGS}"
                "-DLD_FLAGS=${IL_X64_LD_FLAGS}"
                -DMODE=syntax
                -P ${CODEGEN_E2E})
        zanna_set_test_labels(codegen_syntax_only codegen syntax_only)
        set_tests_properties(codegen_syntax_only PROPERTIES RESOURCE_LOCK codegen_e2e_workdir)
    else ()
        message(STATUS "Skipping codegen_* tests (CLANG not found or flags OFF)")
    endif ()

    if (IL_ENABLE_X64_ASM_ASSEMBLE_LINK AND CLANG_FOUND)
        zanna_add_ctest(codegen_assemble_link
                ${CMAKE_COMMAND}
                "-DCLANG_BIN=${CLANG_EXECUTABLE}"
                "-DASM_FLAGS=${IL_X64_ASM_FLAGS}"
                "-DLD_FLAGS=${IL_X64_LD_FLAGS}"
                -DMODE=assemble_link
                -P ${CODEGEN_E2E})
        zanna_set_test_labels(codegen_assemble_link codegen assemble_link)
        set_tests_properties(codegen_assemble_link PROPERTIES RESOURCE_LOCK codegen_e2e_workdir)
    else ()
        message(STATUS "Skipping codegen_* tests (CLANG not found or flags OFF)")
    endif ()

    if (IL_ENABLE_X64_NATIVE_RUN AND CLANG_FOUND)
        zanna_add_ctest(codegen_native_run
                ${CMAKE_COMMAND}
                "-DCLANG_BIN=${CLANG_EXECUTABLE}"
                "-DASM_FLAGS=${IL_X64_ASM_FLAGS}"
                "-DLD_FLAGS=${IL_X64_LD_FLAGS}"
                -DMODE=run
                -P ${CODEGEN_E2E})
        zanna_set_test_labels(codegen_native_run codegen native_run)
        set_tests_properties(codegen_native_run PROPERTIES RESOURCE_LOCK codegen_e2e_workdir)
    else ()
        message(STATUS "Skipping codegen_* tests (CLANG not found or flags OFF)")
    endif ()

    set(ZANNA_E2E_CODEGEN_DONE TRUE PARENT_SCOPE)
endfunction()

# Namespace multi-file E2E test
if (NOT TARGET test_namespace_e2e)
    zanna_add_test(test_namespace_e2e ${ZANNA_TESTS_DIR}/e2e/test_namespace_e2e.cpp)
    target_link_libraries(test_namespace_e2e PRIVATE fe_basic zanna_support)
    zanna_add_ctest(test_namespace_e2e test_namespace_e2e)
endif ()

# ARM64 native linking tests - only on macOS ARM64
if (CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64" AND APPLE)
    set(_ZANNA_E2E_DIR ${CMAKE_CURRENT_LIST_DIR})

    # ── VM<->native differential gate over the shared IL corpus ──────────────
    # Byte-diffs `zanna -run` (VM) against `zanna codegen arm64 -run-native` for
    # each curated corpus program. Building this gate surfaced four real native
    # codegen divergences; ALL FOUR are now fixed and included below:
    #   - idxchk_normalize: idx.chk normalization (idx - lo) survived post-RA DCE.
    #   - switch_i32: switch/narrow result no longer copy-forwarded to an undef vreg.
    #   - eh_catch_return: trap.kind is rewritten off the handler %err.
    #   - global_scalar_load_store: mutable scalar globals now emit a writable
    #     __DATA,__data section in the binary object path; the text section's
    #     undefined reference coalesces onto the data-section defined global.
    set(_ZANNA_DIFF_CORPUS
            ret_i64 arith_chain block_params_sum conditional_abs recursive_factorial
            alloca_store_load gep_stack bitwise_shift unsigned_div_rem signed_div_rem
            fp_cast_round cast_round_to_even idxchk_normalize i1_bool_flow call_two_args
            string_print print_i64 branch_join_value i16_narrow_store unsigned_compare
            fcmp_ord_nan fcmp_uno_nan err_get_msg_exec switch_i32 eh_catch_return
            global_scalar_load_store
            kernel_loop_sum kernel_array_traverse kernel_checked_arith
            kernel_div_const kernel_switch_dispatch kernel_call_leaf
            kernel_select_diamond)
    foreach (_prog IN LISTS _ZANNA_DIFF_CORPUS)
        zanna_add_ctest(differential_${_prog}
                ${CMAKE_COMMAND}
                -DILC=$<TARGET_FILE:zanna>
                -DARCH=arm64
                -DIL_FILE=${CMAKE_SOURCE_DIR}/src/tests/shared_corpus/il/success/${_prog}.il
                -P ${_ZANNA_E2E_DIR}/differential_vm_native.cmake)
        zanna_set_test_labels(differential_${_prog} arm64 differential)
    endforeach ()

    # Trapping corpus programs through the same VM<->native gate: a null
    # dereference must trap deterministically on both backends (same stdout
    # and exit code), not SIGSEGV natively. These failed with a 139-vs-1
    # exit mismatch before native load/store null-page guards landed.
    set(_ZANNA_DIFF_TRAP_CORPUS trap_null_load trap_null_store)
    foreach (_prog IN LISTS _ZANNA_DIFF_TRAP_CORPUS)
        zanna_add_ctest(differential_${_prog}
                ${CMAKE_COMMAND}
                -DILC=$<TARGET_FILE:zanna>
                -DARCH=arm64
                -DIL_FILE=${CMAKE_SOURCE_DIR}/src/tests/shared_corpus/il/traps/${_prog}.il
                -P ${_ZANNA_E2E_DIR}/differential_vm_native.cmake)
        zanna_set_test_labels(differential_${_prog} arm64 differential)
    endforeach ()

    # Vtris demo - compile, assemble, link
    zanna_add_ctest(arm64_native_vtris
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DRUNTIME_LIB=${CMAKE_BINARY_DIR}/src/runtime/libzanna_runtime.a
            -DREGEX_ENGINE_LIB=$<TARGET_FILE:zanna_regex_engine>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/examples/games/vtris/vtris.bas
            -DDEMO_NAME=vtris
            -P ${_ZANNA_E2E_DIR}/test_arm64_native_link.cmake)
    zanna_set_test_labels(arm64_native_vtris arm64 native_link)

    # Frogger demo - compile, assemble, link
    zanna_add_ctest(arm64_native_frogger
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DRUNTIME_LIB=${CMAKE_BINARY_DIR}/src/runtime/libzanna_runtime.a
            -DREGEX_ENGINE_LIB=$<TARGET_FILE:zanna_regex_engine>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/examples/games/frogger-basic/frogger.bas
            -DDEMO_NAME=frogger
            -P ${_ZANNA_E2E_DIR}/test_arm64_native_link.cmake)
    zanna_set_test_labels(arm64_native_frogger arm64 native_link)

    # Centipede demo - compile, assemble, link
    zanna_add_ctest(arm64_native_centipede
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DRUNTIME_LIB=${CMAKE_BINARY_DIR}/src/runtime/libzanna_runtime.a
            -DREGEX_ENGINE_LIB=$<TARGET_FILE:zanna_regex_engine>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/examples/games/centipede-basic/centipede.bas
            -DDEMO_NAME=centipede
            -P ${_ZANNA_E2E_DIR}/test_arm64_native_link.cmake)
    zanna_set_test_labels(arm64_native_centipede arm64 native_link)

    # Comprehensive tests - ARM64 native execution
    zanna_add_ctest(arm64_comprehensive_data_structures
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DRUNTIME_LIB=${CMAKE_BINARY_DIR}/src/runtime/libzanna_runtime.a
            -DREGEX_ENGINE_LIB=$<TARGET_FILE:zanna_regex_engine>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/comprehensive/data_structures.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/comprehensive/data_structures.bas.out
            -P ${_ZANNA_E2E_DIR}/test_arm64_comprehensive.cmake)
    zanna_set_test_labels(arm64_comprehensive_data_structures arm64 native_link comprehensive)

    zanna_add_ctest(arm64_comprehensive_oop_features
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DRUNTIME_LIB=${CMAKE_BINARY_DIR}/src/runtime/libzanna_runtime.a
            -DREGEX_ENGINE_LIB=$<TARGET_FILE:zanna_regex_engine>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/comprehensive/oop_features.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/comprehensive/oop_features.bas.out
            -P ${_ZANNA_E2E_DIR}/test_arm64_comprehensive.cmake)
    zanna_set_test_labels(arm64_comprehensive_oop_features arm64 native_link comprehensive)

    zanna_add_ctest(arm64_comprehensive_control_flow_strings
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DRUNTIME_LIB=${CMAKE_BINARY_DIR}/src/runtime/libzanna_runtime.a
            -DREGEX_ENGINE_LIB=$<TARGET_FILE:zanna_regex_engine>
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/comprehensive/control_flow_strings.bas
            -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/comprehensive/control_flow_strings.bas.out
            -P ${_ZANNA_E2E_DIR}/test_arm64_comprehensive.cmake)
    zanna_set_test_labels(arm64_comprehensive_control_flow_strings arm64 native_link comprehensive)
endif ()

# Comprehensive tests - VM execution (cross-platform)
zanna_add_ctest(vm_comprehensive_data_structures
        ${CMAKE_COMMAND}
        -DILC=$<TARGET_FILE:zanna>
        -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/comprehensive/data_structures.bas
        -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/comprehensive/data_structures.bas.out
        -P ${CMAKE_SOURCE_DIR}/src/tests/e2e/test_basic_math_phase1.cmake)
zanna_set_test_labels(vm_comprehensive_data_structures vm comprehensive)

zanna_add_ctest(vm_comprehensive_oop_features
        ${CMAKE_COMMAND}
        -DILC=$<TARGET_FILE:zanna>
        -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/comprehensive/oop_features.bas
        -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/comprehensive/oop_features.bas.out
        -P ${CMAKE_SOURCE_DIR}/src/tests/e2e/test_basic_math_phase1.cmake)
zanna_set_test_labels(vm_comprehensive_oop_features vm comprehensive)

zanna_add_ctest(vm_comprehensive_control_flow_strings
        ${CMAKE_COMMAND}
        -DILC=$<TARGET_FILE:zanna>
        -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/comprehensive/control_flow_strings.bas
        -DGOLDEN=${CMAKE_SOURCE_DIR}/src/tests/e2e/comprehensive/control_flow_strings.bas.out
        -P ${CMAKE_SOURCE_DIR}/src/tests/e2e/test_basic_math_phase1.cmake)
zanna_set_test_labels(vm_comprehensive_control_flow_strings vm comprehensive)

# ── Mixed-language interop (ADR 0268) ────────────────────────────────────────
# Each case runs a `lang mixed` project end to end and compares stdout. They pin
# the contract documented in docs/languages/interop.md: symbols spelled the same
# way on both sides resolve in either direction, and a library module's
# top-level initialization still runs.
foreach (_interop_case basic_entry zia_entry library_init)
    zanna_add_ctest(interop_mixed_${_interop_case}
            ${CMAKE_COMMAND}
            -DILC=$<TARGET_FILE:zanna>
            -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/src/tests/e2e/interop/${_interop_case}
            -DEXPECT_FILE=${CMAKE_SOURCE_DIR}/src/tests/e2e/interop/${_interop_case}/expected.out
            -P ${CMAKE_SOURCE_DIR}/src/tests/e2e/test_interop_project.cmake)
    zanna_set_test_labels(interop_mixed_${_interop_case} e2e vm)
endforeach ()
