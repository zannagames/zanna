# SPDX-License-Identifier: GPL-3.0-only
# File: tests/golden/CMakeLists.txt
# Purpose: Golden tests for BASIC diagnostics, runtime output, and IL optimizations.
# Uses helper functions from TestHelpers.cmake to reduce boilerplate.

function(zanna_add_golden_suite_tests)
    if (TEST basic_error_unknown_var)
        return()
    endif ()

    set(_ZANNA_GOLDEN_DIR ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
    set(BASIC_ILC $<TARGET_FILE:zanna>)

    # ==================================================================
    # BASIC compile-error diagnostics (check_error.cmake)
    # ==================================================================
    set(_BE ${_ZANNA_GOLDEN_DIR}/basic_errors)

    _golden_error(basic_error_unknown_var ${_BE} unknown_var)
    _golden_error(basic_error_mismatched_next ${_BE} mismatched_next)
    _golden_error(basic_error_exit_outside_loop ${_BE} exit_outside_loop)
    _golden_error(basic_error_exit_mismatch ${_BE} exit_mismatch)
    _golden_error(basic_error_loop_var_assignment ${_BE} loop_var_assignment)
    _golden_error(basic_error_loop_var_input ${_BE} loop_var_input)
    _golden_error(basic_error_bad_goto ${_BE} bad_goto)
    _golden_error(basic_error_const_div_zero ${_BE} const_div_zero)
    _golden_error(basic_error_const_divmod_zero ${_BE} const_divmod_zero)
    _golden_error(basic_error_string_divmod ${_BE} string_divmod)
    _golden_error(basic_error_bad_string_intrinsics ${_BE} bad_string_intrinsics)
    _golden_error(basic_error_try_missing_catch ${_BE} try_missing_catch)
    _golden_error(basic_error_catch_without_try ${_BE} catch_without_try)
    _golden_error(basic_error_float_to_int ${_BE} float_to_int EXPECT_EXIT_ZERO)
    _golden_error(basic_error_float_in_string ${_BE} float_in_string)
    _golden_error(basic_error_bad_conversions ${_BE} bad_conversions)
    _golden_error(basic_error_obj_to_int_assign ${_BE} obj_to_int_assign)
    _golden_error(basic_error_obj_param_primitive_arg ${_BE} obj_param_primitive_arg)
    _golden_error(basic_error_bad_math_funcs ${_BE} bad_math_funcs)
    _golden_error(basic_error_private_method_access ${_BE} private_method_access)
    _golden_error(basic_error_private_field_access ${_BE} private_field_access)
    _golden_error(basic_error_namespace_notfound ${_BE} namespace_notfound)
    _golden_error(basic_error_namespace_ambiguous ${_BE} namespace_ambiguous)
    _golden_error(basic_error_namespace_duplicate_alias ${_BE} namespace_duplicate_alias)
    _golden_error(basic_error_using_after_decl ${_BE} using_after_decl)
    _golden_error(basic_error_namespace_case_insensitive_duplicate ${_BE} namespace_case_insensitive_duplicate)
    _golden_error(basic_error_reserved_root_user_decl ${_BE} reserved_root_user_decl)
    _golden_error(basic_error_reserved_root_decl ${_BE} reserved_root_decl)

    # '+' concatenation is allowed; ensure no error for string arithmetic.
    zanna_add_ctest(basic_error_string_arith
            ${CMAKE_COMMAND} -DILC=${BASIC_ILC}
            -DBAS_FILE=${_BE}/string_arith.bas -DEXPECT_EXIT_ZERO=TRUE
            -P ${_ZANNA_GOLDEN_DIR}/basic_errors/check_error.cmake)

    # USING inside namespace blocks is allowed; ensure no error.
    zanna_add_ctest(basic_using_in_namespace_scoped_ok
            ${CMAKE_COMMAND} -DILC=${BASIC_ILC}
            -DBAS_FILE=${_BE}/using_in_namespace.bas -DEXPECT_EXIT_ZERO=TRUE
            -P ${_ZANNA_GOLDEN_DIR}/basic_errors/check_error.cmake)

    # Runtime namespaces enabled: USING Zanna.* is allowed; expect successful compile
    zanna_add_ctest(basic_error_reserved_root_user_using
            ${CMAKE_COMMAND} -DILC=${BASIC_ILC}
            -DBAS_FILE=${_BE}/reserved_root_user_using.bas -DEXPECT_EXIT_ZERO=TRUE
            -P ${_ZANNA_GOLDEN_DIR}/basic_errors/check_error.cmake)

    # Inline-expect error tests (EXPECT string passed directly, no .stderr file)
    zanna_add_ctest(basic_error_ambiguous_using_call
            ${CMAKE_COMMAND} -DILC=${BASIC_ILC}
            -DBAS_FILE=${_BE}/ambiguous_using_call.bas
            "-DEXPECT=ambiguous procedure 'PRINTI64'.* matches: My\\.Console\\.PRINTI64, Zanna\\.Terminal\\.PRINTI64"
            -P ${_ZANNA_GOLDEN_DIR}/basic_errors/check_error.cmake)

    zanna_add_ctest(basic_error_shadow_builtin_decl
            ${CMAKE_COMMAND} -DILC=${BASIC_ILC}
            -DBAS_FILE=${_BE}/shadow_builtin_decl.bas "-DEXPECT=E_ZANNA_BUILTIN_SHADOW"
            -P ${_ZANNA_GOLDEN_DIR}/basic_errors/check_error.cmake)

    # ------------------------------------------------------------------
    # BASIC examples (smoke): ensure these compile successfully
    # ------------------------------------------------------------------
    zanna_add_ctest(example_basic_addfile
            ${CMAKE_COMMAND} -DILC=${BASIC_ILC}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/examples/zbasic/addfile_main.bas -DEXPECT_EXIT_ZERO=TRUE
            -P ${_ZANNA_GOLDEN_DIR}/basic_errors/check_error.cmake)

    zanna_add_ctest(example_basic_addfile_ns
            ${CMAKE_COMMAND} -DILC=${BASIC_ILC}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/examples/zbasic/addfile_ns_main.bas -DEXPECT_EXIT_ZERO=TRUE
            -P ${_ZANNA_GOLDEN_DIR}/basic_errors/check_error.cmake)

    # ------------------------------------------------------------------
    # OOP error diagnostics
    # ------------------------------------------------------------------
    _golden_error(oop_error_overload_ambiguous
            ${CMAKE_SOURCE_DIR}/src/tests/golden/oop/errors OverloadAmbiguous)
    _golden_error(oop_error_property_accessor_access
            ${CMAKE_SOURCE_DIR}/src/tests/golden/oop/errors PropertyAccessorAccess)

    set(OOP_ERR_DIR ${_ZANNA_GOLDEN_DIR}/oop_errors)
    _golden_error(oop_error_override_nonvirtual ${OOP_ERR_DIR} override_nonvirtual)
    _golden_error(oop_error_signature_mismatch ${OOP_ERR_DIR} signature_mismatch)
    _golden_error(oop_error_final_override ${OOP_ERR_DIR} final_override)
    _golden_error(oop_error_abstract_instantiate ${OOP_ERR_DIR} abstract_instantiate)
    _golden_error(oop_error_base_not_found ${OOP_ERR_DIR} base_not_found)

    # ------------------------------------------------------------------
    # Type soundness golden tests
    # ------------------------------------------------------------------
    _golden_error(basic_error_ts_int_to_string ${_BE} ts_int_to_string)
    _golden_error(basic_error_ts_wrong_sub_arg ${_BE} ts_wrong_sub_arg)
    _golden_error(basic_error_ts_wrong_arity ${_BE} ts_wrong_arity)
    _golden_error(basic_error_ts_string_comparison ${_BE} ts_string_comparison)
    _golden_error(basic_error_ts_undef_var ${_BE} ts_undef_var)
    _golden_error(basic_error_ts_const_assign ${_BE} ts_const_assign)
    _golden_error(basic_error_ts_bool_arith ${_BE} ts_bool_arith)
    _golden_error(basic_error_ts_string_to_int ${_BE} ts_string_to_int)
    _golden_error(basic_error_ts_wrong_ret_type ${_BE} ts_wrong_ret_type)
    _golden_error(basic_error_ts_float_divmod ${_BE} ts_float_divmod EXPECT_EXIT_ZERO)

    # Runtime class readonly property error
    _golden_error(runtime_classes_readonly_prop_set
            ${_ZANNA_GOLDEN_DIR}/runtime_classes readonly_prop_set)

    # ==================================================================
    # BASIC run + stdout comparison (check_basic_run_output.cmake)
    # ==================================================================

    # OOP runtime e2e
    set(_OOP_RT ${_ZANNA_GOLDEN_DIR}/oop_runtime)
    _golden_basic_run(oop_runtime_file_and_stringbuilder ${_OOP_RT} file_and_stringbuilder file_and_stringbuilder.stdout)
    _golden_basic_run(oop_runtime_list_counts ${_OOP_RT} list_counts list_counts.stdout)
    _golden_basic_run(oop_runtime_list_ops ${_OOP_RT} list_ops list_ops.stdout)
    _golden_basic_run(oop_runtime_seq_ops ${_OOP_RT} seq_ops seq_ops.stdout)
    _golden_basic_run(oop_runtime_map_ops ${_OOP_RT} map_ops map_ops.stdout)
    _golden_basic_run(oop_runtime_file_exists_toggle ${_OOP_RT} file_exists_toggle file_exists_toggle.stdout
            CLEANUP_FILE ${CMAKE_CURRENT_BINARY_DIR}/tmp_exists.txt)
    _golden_basic_run(oop_runtime_io_file_helpers ${_OOP_RT} io_file_helpers io_file_helpers.stdout)
    _golden_basic_run(oop_runtime_dir_seq_wrappers ${_OOP_RT} dir_seq_wrappers dir_seq_wrappers.stdout)
    _golden_basic_run(oop_runtime_user_class_tostring ${_OOP_RT} user_class_tostring user_class_tostring.stdout)

    # Runtime classes
    set(_RC ${_ZANNA_GOLDEN_DIR}/runtime_classes)
    _golden_basic_run(runtime_classes_string_props ${_RC} string_props string_props.stdout)
    _golden_basic_run(runtime_classes_string_methods ${_RC} string_methods string_methods.stdout)
    _golden_basic_run(runtime_classes_sb_basic ${_RC} sb_basic sb_basic.stdout)
    _golden_basic_run(runtime_classes_string_alias ${_RC} string_alias string_alias.stdout)

    # Arrays
    set(_ARR ${_ZANNA_GOLDEN_DIR}/arrays)
    _golden_basic_run(basic_arrays_assign_alias ${_ARR} assign_alias assign_alias.stdout)
    _golden_basic_run(basic_arrays_resize_breaks_share ${_ARR} resize_breaks_share resize_breaks_share.stdout)
    _golden_basic_run(basic_arrays_string_store_print ${_ARR} string_array_store_and_print string_array_store_and_print.stdout)
    _golden_basic_run(basic_arrays_string_overwrite ${_ARR} string_array_overwrite string_array_overwrite.stdout)

    # OOP golden (virtual dispatch, inheritance, interfaces)
    set(_OOP ${_ZANNA_GOLDEN_DIR}/oop)
    _golden_basic_run(oop_virtual_dispatch ${_OOP} virtual_dispatch virtual_dispatch.stdout)
    _golden_basic_run(oop_base_qualifier ${_OOP} base_qualifier base_qualifier.stdout)
    _golden_basic_run(oop_virtual_no_override ${_OOP} virtual_no_override virtual_no_override.stdout)
    _golden_basic_run(oop_interface_basic ${_OOP} interface_basic interface_basic.stdout)
    _golden_basic_run(oop_method_sub_call ${_OOP} method_sub_call method_sub_call.stdout)
    _golden_basic_run(oop_mutate_and_return ${_OOP} mutate_and_return mutate_and_return.stdout)
    _golden_basic_run(oop_array_of_objects ${_ZANNA_GOLDEN_DIR}/basic/oop array_of_objects array_of_objects.stdout)
    _golden_basic_run(basic_oop_function_returns_class ${_OOP} function_returns_class function_returns_class.stdout)
    _golden_basic_run(oop_is_as_operators ${_ZANNA_GOLDEN_DIR}/basic/oop is_as_operators is_as_operators.stdout)

    # BASIC general
    set(_BAS ${_ZANNA_GOLDEN_DIR}/basic)
    _golden_basic_run(basic_print_commas_alignment ${_BAS} print_commas print_commas.stdout)
    _golden_basic_run(basic_namespace_simple ${_BAS} namespace_simple namespace_simple.stdout)
    _golden_basic_run(basic_namespace_case_insensitive ${_BAS} namespace_case_insensitive namespace_case_insensitive.stdout)
    _golden_basic_run(basic_namespace_using ${_BAS} namespace_using namespace_using.stdout)
    _golden_basic_run(basic_namespace_inheritance ${_BAS} namespace_inheritance namespace_inheritance.stdout)
    _golden_basic_run(basic_zanna_root_example ${_BAS} zanna_root_example zanna_root_example.stdout)
    _golden_basic_run(basic_do_gosub_loop ${_BAS} do_gosub_loop do_gosub_loop.stdout)
    _golden_basic_run(basic_int_truthy_condition ${_BAS} int_truthy int_truthy.stdout)
    _golden_basic_run(basic_shared_var ${_BAS} shared_var shared_var.stdout)
    _golden_basic_run(basic_not_eof_loop ${_BAS} not_eof_loop not_eof_loop.stdout)

    # TRY/CATCH golden tests
    set(_TC ${_ZANNA_GOLDEN_DIR}/trycatch)
    _golden_basic_run(basic_trycatch_div_zero ${_TC} try_div_zero try_div_zero.stdout)
    _golden_basic_run(basic_trycatch_nested_first_catches ${_TC} nested_try_first_catches nested_try_first_catches.stdout)

    # BASIC runtime namespace tests
    set(_BRN ${_ZANNA_GOLDEN_DIR}/basic_runtime_ns)
    _golden_basic_run(basic_runtime_ns_print_using ${_BRN} print_using print_using.stdout)
    _golden_basic_run(basic_runtime_ns_print_qualified ${_BRN} print_qualified print_qualified.stdout)
    _golden_basic_run(basic_runtime_ns_using_in_namespace ${_BRN} using_in_namespace using_in_namespace.stdout)
    _golden_basic_run(basic_runtime_ns_strings_len ${_BRN} strings_len strings_len.stdout)
    _golden_basic_run(basic_runtime_ns_environment_variables ${_BRN} environment_variables environment_variables.stdout)
    _golden_basic_run(basic_runtime_ns_diagnostics_assert_success ${_BRN} diagnostics_assert_success diagnostics_assert_success.stdout)

    # BASIC run error tests (check_basic_run_error.cmake)
    zanna_add_ctest(basic_runtime_ns_diagnostics_assert_trap_message
            ${CMAKE_COMMAND} -DILC=${BASIC_ILC}
            -DBAS_FILE=${_BRN}/diagnostics_assert_trap_msg.bas "-DEXPECT=boom"
            -P ${_ZANNA_GOLDEN_DIR}/basic_errors/check_basic_run_error.cmake)

    zanna_add_ctest(basic_runtime_ns_diagnostics_assert_trap_default
            ${CMAKE_COMMAND} -DILC=${BASIC_ILC}
            -DBAS_FILE=${_BRN}/diagnostics_assert_trap_default.bas "-DEXPECT=Assertion failed"
            -P ${_ZANNA_GOLDEN_DIR}/basic_errors/check_basic_run_error.cmake)

    # ==================================================================
    # VM trap location tests (check_vm_trap_loc.cmake)
    # ==================================================================
    file(READ ${_ZANNA_GOLDEN_DIR}/vm_trap_loc.stderr _expect_trap_loc)
    string(STRIP "${_expect_trap_loc}" _expect_trap_loc)
    _golden_vm_trap_loc(vm_trap_loc ${_ZANNA_GOLDEN_DIR}/vm_trap_loc.il "${_expect_trap_loc}")

    zanna_add_ctest(vm_trap_overflow_message
            ${CMAKE_COMMAND} -DILC=${BASIC_ILC}
            -DIL_FILE=${_ZANNA_GOLDEN_DIR}/vm_trap_overflow.il
            -P ${_ZANNA_GOLDEN_DIR}/check_vm_trap_overflow.cmake)

    set(NUMERICS_IL_DIR ${_ZANNA_GOLDEN_DIR}/numerics_il)
    _golden_vm_trap_loc(numerics_il_i16_add_overflow ${NUMERICS_IL_DIR}/i16_add_overflow.il "Overflow")
    _golden_vm_trap_loc(numerics_il_int_div_min_overflow ${NUMERICS_IL_DIR}/int_div_min_overflow.il "Overflow")

    # ==================================================================
    # IL run + stdout comparison (check_run_output.cmake)
    # ==================================================================
    _golden_il_run(numerics_il_i16_mul_ok ${NUMERICS_IL_DIR}/i16_mul_ok.il "30000")

    string(JOIN "\n" _expect_mod "-1" "1" "-1")
    _golden_il_run(numerics_il_mod_sign ${NUMERICS_IL_DIR}/mod_sign.il "${_expect_mod}")

    set(ERRORS_MAP_DIR ${_ZANNA_GOLDEN_DIR}/errors_map)
    _golden_il_run_file(errors_map_print_kind_from_handler ${ERRORS_MAP_DIR}/print_kind_from_handler.il ${ERRORS_MAP_DIR}/print_kind_expected.out)
    _golden_il_run_file(errors_map_div_zero ${ERRORS_MAP_DIR}/div_zero.il ${ERRORS_MAP_DIR}/div_zero_expected.out)
    _golden_il_run_file(errors_map_cast_invalid ${ERRORS_MAP_DIR}/cast_invalid.il ${ERRORS_MAP_DIR}/cast_invalid_expected.out)
    _golden_il_run_file(errors_map_idx_oob ${ERRORS_MAP_DIR}/idx_oob.il ${ERRORS_MAP_DIR}/idx_oob_expected.out)
    _golden_il_run_file(errors_map_from_err_fnf ${ERRORS_MAP_DIR}/from_err_fnf.il ${ERRORS_MAP_DIR}/from_err_fnf_expected.out)
    _golden_il_run_file(errors_map_from_err ${ERRORS_MAP_DIR}/from_err.il ${ERRORS_MAP_DIR}/expected.out)

    set(EH_RUNTIME_DIR ${_ZANNA_GOLDEN_DIR}/eh_runtime)
    _golden_il_run_file(eh_runtime_div_by_zero_with_handler ${EH_RUNTIME_DIR}/div_by_zero_with_handler.il ${EH_RUNTIME_DIR}/div_by_zero_with_handler.out)
    _golden_il_run_file(eh_runtime_bounds_with_handler ${EH_RUNTIME_DIR}/bounds_with_handler.il ${EH_RUNTIME_DIR}/bounds_with_handler.out)

    _golden_il_run_file(il_quickstart_example ${_ZANNA_GOLDEN_DIR}/quickstart.il ${_ZANNA_GOLDEN_DIR}/quickstart.stdout)

    # `zanna run file.il` must execute IL like the legacy `-run` form (VDOC-013).
    file(READ ${_ZANNA_GOLDEN_DIR}/quickstart.stdout _expect_run_sub)
    string(STRIP "${_expect_run_sub}" _expect_run_sub)
    zanna_add_ctest(il_quickstart_run_subcommand
            ${CMAKE_COMMAND} -DILC=${BASIC_ILC}
            -DIL_FILE=${_ZANNA_GOLDEN_DIR}/quickstart.il
            "-DEXPECT=${_expect_run_sub}" -DRUN_CMD=run
            -P ${_ZANNA_GOLDEN_DIR}/check_run_output.cmake)
    _golden_il_run_file(il_zanna_ns_print_i64 ${_ZANNA_GOLDEN_DIR}/il/zanna_namespaces/print_i64.il ${_ZANNA_GOLDEN_DIR}/il/zanna_namespaces/print_i64.stdout)

    # When dual-publish is enabled, legacy rt_* alias should still work
    if (ZANNA_RUNTIME_NS_DUAL)
        file(READ ${_ZANNA_GOLDEN_DIR}/il/zanna_namespaces/print_i64.stdout _expect_ns_i64)
        string(STRIP "${_expect_ns_i64}" _expect_ns_i64)
        _golden_il_run(il_legacy_rt_print_i64
                ${_ZANNA_GOLDEN_DIR}/il/zanna_namespaces/print_i64_legacy.il "${_expect_ns_i64}")
    endif ()

    # ==================================================================
    # Exit code tests (check_run_exit_code.cmake)
    # ==================================================================
    _golden_exit_code(il_environment_end_program
            IL_FILE ${_ZANNA_GOLDEN_DIR}/il/environment_end_program.il EXIT_CODE 7)
    _golden_exit_code(basic_runtime_ns_environment_end_program
            BAS_FILE ${_BRN}/environment_end_program.bas EXIT_CODE 7)

    # ==================================================================
    # IL optimizer golden tests (check_opt.cmake)
    # ==================================================================
    set(_OPT ${_ZANNA_GOLDEN_DIR}/il_opt)
    _golden_il_opt(il_opt_const_if ${_OPT}/const_if.in.il ${_OPT}/const_if.opt.il)
    _golden_il_opt(il_opt_arith_id ${_OPT}/arith_id.in.il ${_OPT}/arith_id.opt.il)
    _golden_il_opt(il_opt_math_constfold ${_OPT}/math_constfold.in.il ${_OPT}/math_constfold.opt.il PASSES constfold)
    _golden_il_opt(il_opt_cbr_same_target ${_OPT}/cbr_same_target.in.il ${_OPT}/cbr_same_target.golden PASSES peephole)
    zanna_set_test_labels(il_opt_cbr_same_target smoke ilopt)
    _golden_il_opt(il_opt_mem2reg_simple ${_OPT}/mem2reg_simple.in.il ${_OPT}/mem2reg_simple.opt.il PASSES mem2reg)
    _golden_il_opt(il_opt_mem2reg_diamond ${_OPT}/mem2reg_diamond.in.il ${_OPT}/mem2reg_diamond.opt.il PASSES mem2reg)
    _golden_il_opt(il_opt_mem2reg_nested ${_OPT}/mem2reg_nested.in.il ${_OPT}/mem2reg_nested.opt.il PASSES mem2reg)
    _golden_il_opt(il_opt_mem2reg_loop ${_OPT}/mem2reg_loop.in.il ${_OPT}/mem2reg_loop.opt.il PASSES mem2reg)
    _golden_il_opt(il_opt_mem2reg_multi ${_OPT}/mem2reg_multi.in.il ${_OPT}/mem2reg_multi.opt.il PASSES mem2reg)

    # ==================================================================
    # Constant folding golden tests (check_constfold.cmake)
    # ==================================================================
    set(_CF ${_ZANNA_GOLDEN_DIR}/constfold)
    zanna_add_ctest(constfold_add_overflow
            ${CMAKE_COMMAND} -DILC=${BASIC_ILC}
            -DIL_FILE=${_CF}/constfold_add_overflow.il -DGOLDEN=${_CF}/constfold_add_overflow.opt.il
            -DEXPECT_NONZERO=1 -DEXPECT_REGEX=Overflow
            -P ${_CF}/check_constfold.cmake)
    zanna_add_ctest(constfold_div_by_zero
            ${CMAKE_COMMAND} -DILC=${BASIC_ILC}
            -DIL_FILE=${_CF}/constfold_div_by_zero.il -DGOLDEN=${_CF}/constfold_div_by_zero.opt.il
            -DEXPECT_NONZERO=1 -DEXPECT_REGEX=DivideByZero
            -P ${_CF}/check_constfold.cmake)
    zanna_add_ctest(constfold_rounding
            ${CMAKE_COMMAND} -DILC=${BASIC_ILC}
            -DIL_FILE=${_CF}/constfold_rounding.il -DGOLDEN=${_CF}/constfold_rounding.opt.il
            -DEXPECT_RESULT=0 -DSKIP_RUNTIME=1
            -P ${_CF}/check_constfold.cmake)

    # ==================================================================
    # BASIC → IL golden tests (check_il.cmake)
    # ==================================================================
    set(_FIO ${_ZANNA_GOLDEN_DIR}/fileio)
    _golden_basic_to_il(basic_fileio_lowering_open_close
            ${_FIO}/lowering_open_close.bas ${_FIO}/lowering_open_close.il)
    _golden_basic_to_il(basic_fileio_lowering_print_hash_multi
            ${_FIO}/lowering_print_hash_multi.bas ${_FIO}/lowering_print_hash_multi.il)

    # Skip on Windows: GOSUB compilation crashes (to be investigated)
    if (NOT WIN32)
        _golden_basic_to_il(basic_to_il_gosub
                ${_ZANNA_GOLDEN_DIR}/basic_to_il/gosub.bas ${_ZANNA_GOLDEN_DIR}/basic_to_il/gosub.il)
    endif ()

    zanna_add_ctest(basic_to_il_using_exception_cleanup
            ${CMAKE_COMMAND}
            -DILC=${BASIC_ILC}
            -DBAS_FILE=${_ZANNA_GOLDEN_DIR}/basic_to_il/using_exception_cleanup.bas
            "-DMUST_HAVE=eh.push;using_handler;eh.entry;resume.same;@rt_obj_release_check0;@rt_obj_free"
            -P ${_ZANNA_GOLDEN_DIR}/basic_to_il/check_il_contains.cmake)

endfunction()
