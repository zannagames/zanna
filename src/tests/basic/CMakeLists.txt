# SPDX-License-Identifier: GPL-3.0-only
# File: tests/basic/CMakeLists.txt
# Purpose: BASIC frontend unit and golden tests.

set(ZANNA_BASIC_LIBS fe_basic)
set(ZANNA_BASIC_LIBS ${ZANNA_BASIC_LIBS} PARENT_SCOPE)

function(zanna_add_basic_early_tests)
    if (TARGET test_basic_lexer_high_bit)
        return()
    endif ()

    zanna_add_test(test_basic_lexer_high_bit ${ZANNA_TESTS_DIR}/unit/test_basic_lexer_high_bit.cpp)
    target_link_libraries(test_basic_lexer_high_bit PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_lexer_high_bit test_basic_lexer_high_bit)
endfunction()

function(zanna_add_basic_main_tests)
    if (TARGET test_basic_lexer)
        return()
    endif ()

    set(_ZANNA_BASIC_UNIT_DIR ${ZANNA_TESTS_DIR}/unit)

    zanna_add_test(test_basic_lexer ${_ZANNA_BASIC_UNIT_DIR}/test_basic_lexer.cpp)
    target_link_libraries(test_basic_lexer PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_lexer test_basic_lexer)

    zanna_add_test(test_basic_loc ${_ZANNA_BASIC_UNIT_DIR}/test_basic_loc.cpp)
    target_link_libraries(test_basic_loc PRIVATE fe_basic il_build zanna_il_full)
    zanna_add_ctest(test_basic_loc test_basic_loc)

    zanna_add_test(test_basic_expr_loc ${_ZANNA_BASIC_UNIT_DIR}/test_basic_expr_loc.cpp)
    target_link_libraries(test_basic_expr_loc PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_expr_loc test_basic_expr_loc)

    zanna_add_test(test_basic_semantic ${_ZANNA_BASIC_UNIT_DIR}/test_basic_semantic.cpp)
    target_link_libraries(test_basic_semantic PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_semantic test_basic_semantic)

    zanna_add_test(test_basic_lowerer_collect ${_ZANNA_BASIC_UNIT_DIR}/test_basic_lowerer_collect.cpp)
    target_link_libraries(test_basic_lowerer_collect PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_lowerer_collect test_basic_lowerer_collect)

    zanna_add_test(test_basic_lowerer_conversions ${_ZANNA_BASIC_UNIT_DIR}/test_basic_lowerer_conversions.cpp)
    target_link_libraries(test_basic_lowerer_conversions PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_lowerer_conversions test_basic_lowerer_conversions)

    zanna_add_test(test_basic_emit_common ${_ZANNA_BASIC_UNIT_DIR}/test_basic_emit_common.cpp)
    target_link_libraries(test_basic_emit_common PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_emit_common test_basic_emit_common)

    zanna_add_test(test_basic_lowerer_runtime_helpers ${_ZANNA_BASIC_UNIT_DIR}/test_basic_lowerer_runtime_helpers.cpp)
    target_link_libraries(test_basic_lowerer_runtime_helpers PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_lowerer_runtime_helpers test_basic_lowerer_runtime_helpers)

    zanna_add_test(test_basic_lowerer_string_assignment ${_ZANNA_BASIC_UNIT_DIR}/test_basic_lowerer_string_assignment.cpp)
    target_link_libraries(test_basic_lowerer_string_assignment PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_lowerer_string_assignment test_basic_lowerer_string_assignment)

    zanna_add_test(test_basic_print_runtime_property ${_ZANNA_BASIC_UNIT_DIR}/test_basic_print_runtime_property.cpp)
    target_link_libraries(test_basic_print_runtime_property PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_print_runtime_property test_basic_print_runtime_property)

    zanna_add_test(test_basic_lowerer_string_escapes ${_ZANNA_BASIC_UNIT_DIR}/test_basic_lowerer_string_escapes.cpp)
    target_link_libraries(test_basic_lowerer_string_escapes PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_lowerer_string_escapes test_basic_lowerer_string_escapes)

    zanna_add_test(test_basic_lowerer_select_case ${_ZANNA_BASIC_UNIT_DIR}/test_basic_lowerer_select_case.cpp)
    target_link_libraries(test_basic_lowerer_select_case PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_lowerer_select_case test_basic_lowerer_select_case)

    # Ensure SELECT CASE blocks are inserted before exit (BUG-072 regression test)
    zanna_add_test(test_basic_lowerer_select_case_order ${_ZANNA_BASIC_UNIT_DIR}/test_basic_lowerer_select_case_order.cpp)
    target_link_libraries(test_basic_lowerer_select_case_order PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_lowerer_select_case_order test_basic_lowerer_select_case_order)

    zanna_add_test(test_basic_semantic_components ${_ZANNA_BASIC_UNIT_DIR}/test_basic_semantic_components.cpp)
    target_link_libraries(test_basic_semantic_components PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_semantic_components test_basic_semantic_components)

    zanna_add_test(test_basic_enums ${_ZANNA_BASIC_UNIT_DIR}/test_basic_enums.cpp)
    target_link_libraries(test_basic_enums PRIVATE ${ZANNA_BASIC_LIBS} zanna_test_common)
    zanna_add_ctest(test_basic_enums test_basic_enums)

    zanna_add_test(test_basic_semantic_arrays ${_ZANNA_BASIC_UNIT_DIR}/test_basic_semantic_arrays.cpp)
    target_link_libraries(test_basic_semantic_arrays PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_semantic_arrays test_basic_semantic_arrays)

    zanna_add_test(
            test_basic_semantic_proc_bool_array
            ${_ZANNA_BASIC_UNIT_DIR}/test_basic_semantic_proc_bool_array.cpp)
    target_link_libraries(test_basic_semantic_proc_bool_array PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(
            test_basic_semantic_proc_bool_array test_basic_semantic_proc_bool_array)

    zanna_add_test(test_basic_intrinsic_semantics ${_ZANNA_BASIC_UNIT_DIR}/test_basic_intrinsic_semantics.cpp)
    target_link_libraries(test_basic_intrinsic_semantics PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_intrinsic_semantics test_basic_intrinsic_semantics)

    zanna_add_test(test_basic_diagnostic ${_ZANNA_BASIC_UNIT_DIR}/test_basic_diagnostic.cpp)
    target_link_libraries(test_basic_diagnostic PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_diagnostic test_basic_diagnostic)

    zanna_add_test(test_basic_constfold ${_ZANNA_BASIC_UNIT_DIR}/test_basic_constfold.cpp)
    target_link_libraries(test_basic_constfold PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_constfold test_basic_constfold)

    zanna_add_test(test_basic_compiler ${_ZANNA_BASIC_UNIT_DIR}/test_basic_compiler.cpp)
    target_link_libraries(test_basic_compiler PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_compiler test_basic_compiler)

    zanna_add_test(test_basic_function_as_type ${_ZANNA_BASIC_UNIT_DIR}/test_basic_function_as_type.cpp)
    target_link_libraries(test_basic_function_as_type PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_function_as_type test_basic_function_as_type)

    zanna_add_test(test_basic_ast_mutation ${_ZANNA_BASIC_UNIT_DIR}/test_basic_ast_mutation.cpp)
    target_link_libraries(test_basic_ast_mutation PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_ast_mutation test_basic_ast_mutation)

    zanna_add_test(test_basic_parse_array_var ${_ZANNA_BASIC_UNIT_DIR}/test_basic_parse_array_var.cpp)
    target_link_libraries(test_basic_parse_array_var PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_parse_array_var test_basic_parse_array_var)

    zanna_add_test(test_basic_parse_loops ${_ZANNA_BASIC_UNIT_DIR}/test_basic_parse_loops.cpp)
    target_link_libraries(test_basic_parse_loops PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_parse_loops test_basic_parse_loops)

    zanna_add_test(test_basic_for_loop_ctrl_diagnostics ${_ZANNA_BASIC_UNIT_DIR}/test_basic_for_loop_ctrl_diagnostics.cpp)
    target_link_libraries(test_basic_for_loop_ctrl_diagnostics PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_for_loop_ctrl_diagnostics test_basic_for_loop_ctrl_diagnostics)

    zanna_add_test(test_basic_parse_do ${_ZANNA_BASIC_UNIT_DIR}/test_basic_parse_do.cpp)
    target_link_libraries(test_basic_parse_do PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_parse_do test_basic_parse_do)

    zanna_add_test(test_basic_parse_oop ${_ZANNA_BASIC_UNIT_DIR}/test_basic_parse_oop.cpp)
    target_link_libraries(test_basic_parse_oop PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_parse_oop test_basic_parse_oop)

    zanna_add_test(test_frontends_basic_statement_context ${ZANNA_TESTS_DIR}/frontends/basic/ParserStatementContextTests.cpp)
    target_link_libraries(test_frontends_basic_statement_context PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_frontends_basic_statement_context test_frontends_basic_statement_context)

    zanna_add_test(test_frontends_basic_parse_on_error ${ZANNA_TESTS_DIR}/frontends/basic/ParseOnErrorTests.cpp)
    target_link_libraries(test_frontends_basic_parse_on_error PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_frontends_basic_parse_on_error test_frontends_basic_parse_on_error)

    zanna_add_test(test_frontends_basic_parse_file_io ${ZANNA_TESTS_DIR}/frontends/basic/ParseFileIoTests.cpp)
    target_link_libraries(test_frontends_basic_parse_file_io PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_frontends_basic_parse_file_io test_frontends_basic_parse_file_io)

    zanna_add_test(test_frontends_basic_parse_call_stmt ${ZANNA_TESTS_DIR}/frontends/basic/ParseCallStatementTests.cpp)
    target_link_libraries(test_frontends_basic_parse_call_stmt PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_frontends_basic_parse_call_stmt test_frontends_basic_parse_call_stmt)

    zanna_add_test(
            test_frontends_basic_parse_method_call_stmt
            ${ZANNA_TESTS_DIR}/frontends/basic/ParseMethodCallStatementTests.cpp)
    target_link_libraries(test_frontends_basic_parse_method_call_stmt PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(
            test_frontends_basic_parse_method_call_stmt
            test_frontends_basic_parse_method_call_stmt)

    zanna_add_test(
            test_frontends_basic_lowerer_select_case
            ${ZANNA_TESTS_DIR}/frontends/basic/LowererSelectCaseTests.cpp)
    target_link_libraries(test_frontends_basic_lowerer_select_case PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(
            test_frontends_basic_lowerer_select_case test_frontends_basic_lowerer_select_case)

    # END inside SUB lowering should verify (trap-based termination)
    zanna_add_test(
            test_frontends_basic_lowerer_end_in_sub
            ${ZANNA_TESTS_DIR}/frontends/basic/LowererEndInSubTests.cpp)
    target_link_libraries(test_frontends_basic_lowerer_end_in_sub PRIVATE ${ZANNA_BASIC_LIBS} zanna_il_verify)
    zanna_add_ctest(
            test_frontends_basic_lowerer_end_in_sub
            test_frontends_basic_lowerer_end_in_sub)

    zanna_add_test(
            test_frontends_basic_lowerer_oop_default_ctor
            ${ZANNA_TESTS_DIR}/frontends/basic/LowererOopConstructorTests.cpp)
    target_link_libraries(test_frontends_basic_lowerer_oop_default_ctor PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(
            test_frontends_basic_lowerer_oop_default_ctor
            test_frontends_basic_lowerer_oop_default_ctor)

    zanna_add_test(test_frontends_basic_parse_goto ${ZANNA_TESTS_DIR}/frontends/basic/ParseGotoTests.cpp)
    target_link_libraries(test_frontends_basic_parse_goto PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_frontends_basic_parse_goto test_frontends_basic_parse_goto)

    zanna_add_test(test_frontends_basic_parse_gosub ${ZANNA_TESTS_DIR}/frontends/basic/ParseGosubTests.cpp)
    target_link_libraries(test_frontends_basic_parse_gosub PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_frontends_basic_parse_gosub test_frontends_basic_parse_gosub)

    zanna_add_test(
            test_frontends_basic_parse_type_alias
            ${ZANNA_TESTS_DIR}/frontends/basic/ParseTypeAliasTests.cpp)
    target_link_libraries(test_frontends_basic_parse_type_alias PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(
            test_frontends_basic_parse_type_alias test_frontends_basic_parse_type_alias)

    zanna_add_test(test_frontends_basic_parse_select_case ${ZANNA_TESTS_DIR}/frontends/basic/ParseSelectCaseTests.cpp)
    target_link_libraries(test_frontends_basic_parse_select_case PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(
            test_frontends_basic_parse_select_case test_frontends_basic_parse_select_case)

    zanna_add_test(
            test_frontends_basic_parse_select_case_positive
            ${ZANNA_TESTS_DIR}/frontends/basic/ParseSelectCasePositiveTests.cpp)
    target_link_libraries(test_frontends_basic_parse_select_case_positive PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(
            test_frontends_basic_parse_select_case_positive
            test_frontends_basic_parse_select_case_positive)

    # TRY/CATCH parser unit tests
    zanna_add_test(test_basic_parse_try_catch ${_ZANNA_BASIC_UNIT_DIR}/test_basic_parse_try_catch.cpp)
    target_link_libraries(test_basic_parse_try_catch PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_parse_try_catch test_basic_parse_try_catch)

    zanna_add_test(
            test_frontends_basic_subroutines_gosub_inline_case
            ${ZANNA_TESTS_DIR}/frontends/basic/SubroutinesGosubInlineCaseTests.cpp)
    target_link_libraries(
            test_frontends_basic_subroutines_gosub_inline_case PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(
            test_frontends_basic_subroutines_gosub_inline_case
            test_frontends_basic_subroutines_gosub_inline_case)

    zanna_add_test(
            test_frontends_basic_parse_input_extra_args
            ${ZANNA_TESTS_DIR}/frontends/basic/ParserInputExtraArgsTests.cpp)
    target_link_libraries(test_frontends_basic_parse_input_extra_args PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(
            test_frontends_basic_parse_input_extra_args
            test_frontends_basic_parse_input_extra_args)

    zanna_add_test(
            test_frontends_basic_parse_unknown_stmt
            ${ZANNA_TESTS_DIR}/frontends/basic/ParserUnknownStatementTests.cpp)
    target_link_libraries(test_frontends_basic_parse_unknown_stmt PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(
            test_frontends_basic_parse_unknown_stmt test_frontends_basic_parse_unknown_stmt)

    # Zanna.* runtime calls should resolve in semantics (Terminal/Time)
    zanna_add_test(
            test_frontends_basic_zanna_runtime_calls
            ${ZANNA_TESTS_DIR}/frontends/basic/ZannaRuntimeCallTests.cpp)
    target_link_libraries(test_frontends_basic_zanna_runtime_calls PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(
            test_frontends_basic_zanna_runtime_calls
            test_frontends_basic_zanna_runtime_calls)

    # Paren-less zero-arg method call in statement position is accepted
    zanna_add_test(
            test_frontends_basic_parse_method_call_parenless
            ${ZANNA_TESTS_DIR}/frontends/basic/ParseMethodCallParenlessTests.cpp)
    target_link_libraries(test_frontends_basic_parse_method_call_parenless PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(
            test_frontends_basic_parse_method_call_parenless
            test_frontends_basic_parse_method_call_parenless)

    # ADDFILE directive tests
    zanna_add_test(
            test_frontends_basic_addfile
            ${ZANNA_TESTS_DIR}/frontends/basic/AddFileTests.cpp)
    target_link_libraries(test_frontends_basic_addfile PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_frontends_basic_addfile test_frontends_basic_addfile)

    zanna_add_test(
            test_frontends_basic_line_label_diagnostics
            ${ZANNA_TESTS_DIR}/frontends/basic/ParserLineLabelDiagnosticsTests.cpp)
    target_link_libraries(test_frontends_basic_line_label_diagnostics PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(
            test_frontends_basic_line_label_diagnostics test_frontends_basic_line_label_diagnostics)

    zanna_add_test(test_frontends_basic_lexer_file_io ${ZANNA_TESTS_DIR}/frontends/basic/LexerFileIoTests.cpp)
    target_link_libraries(test_frontends_basic_lexer_file_io PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_frontends_basic_lexer_file_io test_frontends_basic_lexer_file_io)

    zanna_add_test(
            test_frontends_basic_lexer_select_case
            ${ZANNA_TESTS_DIR}/frontends/basic/LexerSelectCaseTests.cpp)
    target_link_libraries(test_frontends_basic_lexer_select_case PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(
            test_frontends_basic_lexer_select_case test_frontends_basic_lexer_select_case)

    zanna_add_test(test_frontends_basic_semantic_exprs ${ZANNA_TESTS_DIR}/frontends/basic/SemanticAnalyzerBinaryExprTests.cpp)
    target_link_libraries(test_frontends_basic_semantic_exprs PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_frontends_basic_semantic_exprs test_frontends_basic_semantic_exprs)

    zanna_add_test(
            test_frontends_basic_semantic_var_expr
            ${ZANNA_TESTS_DIR}/frontends/basic/SemanticAnalyzerVarExprTests.cpp)
    target_link_libraries(test_frontends_basic_semantic_var_expr PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(
            test_frontends_basic_semantic_var_expr test_frontends_basic_semantic_var_expr)

    zanna_add_test(test_frontends_basic_semantic_calls ${ZANNA_TESTS_DIR}/frontends/basic/SemanticAnalyzerCallTests.cpp)
    target_link_libraries(test_frontends_basic_semantic_calls PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_frontends_basic_semantic_calls test_frontends_basic_semantic_calls)

    # Case-insensitive namespace/proc resolution tests
    if (NOT TARGET test_frontends_basic_semantic_case_insensitive)
        zanna_add_test(test_frontends_basic_semantic_case_insensitive ${ZANNA_TESTS_DIR}/frontends/basic/SemanticAnalyzerCaseInsensitiveTests.cpp)
        target_link_libraries(test_frontends_basic_semantic_case_insensitive PRIVATE ${ZANNA_BASIC_LIBS})
        zanna_add_ctest(test_frontends_basic_semantic_case_insensitive test_frontends_basic_semantic_case_insensitive)
    endif ()

    zanna_add_test(
            test_frontends_basic_builtin_registry
            ${ZANNA_TESTS_DIR}/frontends/basic/BuiltinRegistryTests.cpp)
    target_link_libraries(test_frontends_basic_builtin_registry PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(
            test_frontends_basic_builtin_registry test_frontends_basic_builtin_registry)

    zanna_add_test(test_frontends_basic_semantic_on_error ${ZANNA_TESTS_DIR}/frontends/basic/SemanticAnalyzerOnErrorTests.cpp)
    target_link_libraries(test_frontends_basic_semantic_on_error PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_frontends_basic_semantic_on_error test_frontends_basic_semantic_on_error)

    zanna_add_test(
            test_frontends_basic_semantic_call_stmt
            ${ZANNA_TESTS_DIR}/frontends/basic/SemanticAnalyzerCallStatementTests.cpp)
    target_link_libraries(test_frontends_basic_semantic_call_stmt PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(
            test_frontends_basic_semantic_call_stmt test_frontends_basic_semantic_call_stmt)

    zanna_add_test(
            test_frontends_basic_semantic_select_case
            ${ZANNA_TESTS_DIR}/frontends/basic/SemanticAnalyzerSelectCaseTests.cpp)
    target_link_libraries(test_frontends_basic_semantic_select_case PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(
            test_frontends_basic_semantic_select_case test_frontends_basic_semantic_select_case)

    zanna_add_test(test_frontends_basic_semantic_gosub ${ZANNA_TESTS_DIR}/frontends/basic/SemanticAnalyzerGosubTests.cpp)
    target_link_libraries(test_frontends_basic_semantic_gosub PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_frontends_basic_semantic_gosub test_frontends_basic_semantic_gosub)

    zanna_add_test(test_frontends_basic_semantic_file_io ${ZANNA_TESTS_DIR}/frontends/basic/SemanticsFileIoTests.cpp)
    target_link_libraries(test_frontends_basic_semantic_file_io PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_frontends_basic_semantic_file_io test_frontends_basic_semantic_file_io)

    zanna_add_test(test_frontends_basic_type_rules ${ZANNA_TESTS_DIR}/frontends/basic/TypeRulesTests.cpp)
    target_link_libraries(test_frontends_basic_type_rules PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_frontends_basic_type_rules test_frontends_basic_type_rules)

    zanna_add_test(test_basic_intrinsics ${_ZANNA_BASIC_UNIT_DIR}/test_basic_intrinsics.cpp)
    target_link_libraries(test_basic_intrinsics PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_intrinsics test_basic_intrinsics)

    zanna_add_test(test_basic_ast_printer ${_ZANNA_BASIC_UNIT_DIR}/test_basic_ast_printer.cpp)
    target_link_libraries(test_basic_ast_printer PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_ast_printer test_basic_ast_printer)

    zanna_add_test(test_basic_ast_select_case ${_ZANNA_BASIC_UNIT_DIR}/test_basic_ast_select_case.cpp)
    target_link_libraries(test_basic_ast_select_case PRIVATE ${ZANNA_BASIC_LIBS})
    zanna_add_ctest(test_basic_ast_select_case test_basic_ast_select_case)

    zanna_add_test(test_basic_runtime_calls ${ZANNA_TESTS_DIR}/basic/test_basic_runtime_calls.cpp)
    target_link_libraries(test_basic_runtime_calls PRIVATE ${ZANNA_BASIC_LIBS} zanna_test_common)
    zanna_add_ctest(test_basic_runtime_calls test_basic_runtime_calls)
endfunction()

function(zanna_add_basic_ast_golden_tests)
    if (TEST basic_ast_ex1)
        return()
    endif ()

    set(_ZANNA_BASIC_AST_DIR ${ZANNA_TESTS_DIR}/golden/basic_ast)
    set(_ZANNA_BASIC_PARSE_DIR ${ZANNA_TESTS_DIR}/basic/parse)
    set(_ZANNA_BASIC_AST_CHECK ${_ZANNA_BASIC_AST_DIR}/check_ast.cmake)
    set(_ZANNA_BASIC_AST_DUMP $<TARGET_FILE:basic-ast-dump>)

    zanna_add_ctest(basic_ast_ex1
            ${CMAKE_COMMAND}
            -DBASIC_AST_DUMP=${_ZANNA_BASIC_AST_DUMP}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/examples/zbasic/ex1_hello_cond.bas
            -DGOLDEN=${_ZANNA_BASIC_AST_DIR}/ex1_hello_cond.ast
            -P ${_ZANNA_BASIC_AST_CHECK})

    zanna_add_ctest(basic_ast_ex2
            ${CMAKE_COMMAND}
            -DBASIC_AST_DUMP=${_ZANNA_BASIC_AST_DUMP}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/examples/zbasic/ex2_sum_1_to_10.bas
            -DGOLDEN=${_ZANNA_BASIC_AST_DIR}/ex2_sum_1_to_10.ast
            -P ${_ZANNA_BASIC_AST_CHECK})

    zanna_add_ctest(basic_ast_divmod_precedence
            ${CMAKE_COMMAND}
            -DBASIC_AST_DUMP=${_ZANNA_BASIC_AST_DUMP}
            -DBAS_FILE=${_ZANNA_BASIC_AST_DIR}/divmod_precedence.bas
            -DGOLDEN=${_ZANNA_BASIC_AST_DIR}/divmod_precedence.ast
            -P ${_ZANNA_BASIC_AST_CHECK})

    zanna_add_ctest(basic_ast_float_literals
            ${CMAKE_COMMAND}
            -DBASIC_AST_DUMP=${_ZANNA_BASIC_AST_DUMP}
            -DBAS_FILE=${_ZANNA_BASIC_AST_DIR}/float_literals.bas
            -DGOLDEN=${_ZANNA_BASIC_AST_DIR}/float_literals.ast
            -P ${_ZANNA_BASIC_AST_CHECK})

    zanna_add_ctest(basic_ast_functions_subs_parsing
            ${CMAKE_COMMAND}
            -DBASIC_AST_DUMP=${_ZANNA_BASIC_AST_DUMP}
            -DBAS_FILE=${_ZANNA_BASIC_AST_DIR}/functions_subs_parsing.bas
            -DGOLDEN=${_ZANNA_BASIC_AST_DIR}/functions_subs_parsing.ast
            -P ${_ZANNA_BASIC_AST_CHECK})

    zanna_add_ctest(basic_ast_funcs_then_code
            ${CMAKE_COMMAND}
            -DBASIC_AST_DUMP=${_ZANNA_BASIC_AST_DUMP}
            -DBAS_FILE=${_ZANNA_BASIC_PARSE_DIR}/FuncsThenCode.bas
            -DGOLDEN=${_ZANNA_BASIC_PARSE_DIR}/FuncsThenCode.ast
            -P ${_ZANNA_BASIC_AST_CHECK})

    zanna_add_ctest(basic_ast_intrinsic_calls
            ${CMAKE_COMMAND}
            -DBASIC_AST_DUMP=${_ZANNA_BASIC_AST_DUMP}
            -DBAS_FILE=${_ZANNA_BASIC_PARSE_DIR}/IntrinsicCalls.bas
            -DGOLDEN=${_ZANNA_BASIC_PARSE_DIR}/IntrinsicCalls.ast
            -P ${_ZANNA_BASIC_AST_CHECK})

    zanna_add_ctest(basic_ast_boolean_exprs
            ${CMAKE_COMMAND}
            -DBASIC_AST_DUMP=${_ZANNA_BASIC_AST_DUMP}
            -DBAS_FILE=${_ZANNA_BASIC_PARSE_DIR}/test_boolean_exprs.bas
            -DGOLDEN=${_ZANNA_BASIC_PARSE_DIR}/test_boolean_exprs.ast
            -P ${_ZANNA_BASIC_AST_CHECK})

    zanna_add_ctest(basic_ast_pretty_boolean
            ${CMAKE_COMMAND}
            -DBASIC_AST_DUMP=${_ZANNA_BASIC_AST_DUMP}
            -DBAS_FILE=${_ZANNA_BASIC_PARSE_DIR}/test_pretty_boolean.bas
            -DGOLDEN=${_ZANNA_BASIC_PARSE_DIR}/test_pretty_boolean.ast
            -P ${_ZANNA_BASIC_AST_CHECK})

    zanna_add_ctest(basic_ast_future_keywords
            ${CMAKE_COMMAND}
            -DBASIC_AST_DUMP=${_ZANNA_BASIC_AST_DUMP}
            -DBAS_FILE=${_ZANNA_BASIC_PARSE_DIR}/test_future_keywords.bas
            -DGOLDEN=${_ZANNA_BASIC_PARSE_DIR}/test_future_keywords.ast
            -P ${_ZANNA_BASIC_AST_CHECK})
endfunction()

function(zanna_add_basic_lex_golden_tests)
    if (TEST basic_lex_comments)
        return()
    endif ()

    set(_ZANNA_BASIC_LEX_DIR ${ZANNA_TESTS_DIR}/basic/lex)
    set(_ZANNA_BASIC_LEX_DUMP $<TARGET_FILE:basic-lex-dump>)

    zanna_add_ctest(basic_lex_comments
            ${CMAKE_COMMAND}
            -DBASIC_LEX_DUMP=${_ZANNA_BASIC_LEX_DUMP}
            -DBAS_FILE=${_ZANNA_BASIC_LEX_DIR}/Comments.bas
            -DGOLDEN=${_ZANNA_BASIC_LEX_DIR}/Comments.tokens
            -P ${_ZANNA_BASIC_LEX_DIR}/check_tokens.cmake)

    zanna_add_ctest(basic_lex_boolean_tokens
            ${CMAKE_COMMAND}
            -DBASIC_LEX_DUMP=${_ZANNA_BASIC_LEX_DUMP}
            -DBAS_FILE=${_ZANNA_BASIC_LEX_DIR}/test_boolean_tokens.bas
            -DGOLDEN=${_ZANNA_BASIC_LEX_DIR}/test_boolean_tokens.expect
            -P ${_ZANNA_BASIC_LEX_DIR}/check_tokens.cmake)

    zanna_add_ctest(basic_lex_select_case
            ${CMAKE_COMMAND}
            -DBASIC_LEX_DUMP=${_ZANNA_BASIC_LEX_DUMP}
            -DBAS_FILE=${_ZANNA_BASIC_LEX_DIR}/select_case.bas
            -DGOLDEN=${_ZANNA_BASIC_LEX_DIR}/select_case.tokens
            -P ${_ZANNA_BASIC_LEX_DIR}/check_tokens.cmake)

    zanna_add_ctest(basic_lex_ident_with_underscore
            ${CMAKE_COMMAND}
            -DBASIC_LEX_DUMP=${_ZANNA_BASIC_LEX_DUMP}
            -DBAS_FILE=${_ZANNA_BASIC_LEX_DIR}/ident_with_underscore.bas
            -DGOLDEN=${_ZANNA_BASIC_LEX_DIR}/ident_with_underscore.expect
            -P ${_ZANNA_BASIC_LEX_DIR}/check_tokens.cmake)

    zanna_add_ctest(basic_lex_ident_invalid_suffix
            ${CMAKE_COMMAND}
            -DBASIC_LEX_DUMP=${_ZANNA_BASIC_LEX_DUMP}
            -DBAS_FILE=${_ZANNA_BASIC_LEX_DIR}/ident_invalid_suffix.bas
            -DGOLDEN=${_ZANNA_BASIC_LEX_DIR}/ident_invalid_suffix.expect
            -P ${_ZANNA_BASIC_LEX_DIR}/check_tokens.cmake)
endfunction()

function(zanna_add_basic_to_il_golden_tests)
    if (TEST basic_to_il_goldens_batch_0)
        return()
    endif ()
    if (NOT ZANNA_BASIC_TO_IL_GOLDEN_BATCH_SHARDS MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "ZANNA_BASIC_TO_IL_GOLDEN_BATCH_SHARDS must be a positive integer")
    endif ()

    set(_ZANNA_BASIC_ILC $<TARGET_FILE:zanna>)
    set(_ZANNA_BASIC_TO_IL_DIR ${ZANNA_TESTS_DIR}/golden/basic_to_il)
    set(_ZANNA_BASIC_TO_IL_CHECK ${_ZANNA_BASIC_TO_IL_DIR}/check_il.cmake)
    set(_ZANNA_BASIC_TO_IL_CHECK_BOUNDS ${_ZANNA_BASIC_TO_IL_DIR}/check_il_bounds.cmake)
    set(_ZANNA_BASIC_TO_IL_CHECK_CONTAINS ${_ZANNA_BASIC_TO_IL_DIR}/check_il_contains.cmake)
    set(_ZANNA_GOLDEN_SRC_DIR ${ZANNA_TESTS_DIR}/golden/src)
    set(_ZANNA_GOLDEN_IL_DIR ${ZANNA_TESTS_DIR}/golden/il)
    set(_ZANNA_BASIC_GOLDENS_DIR ${ZANNA_TESTS_DIR}/golden/basic_lowering)
    set(_ZANNA_EH_LOWERING_DIR ${ZANNA_TESTS_DIR}/golden/eh_lowering)
    set(_ZANNA_BASIC_TO_IL_BATCH_CASES "")

    macro(_zanna_basic_to_il_add_batch_case case_name case_kind case_bas case_golden case_must_have case_must_not_have)
        if ("${case_kind}" STREQUAL "compare")
            string(APPEND _ZANNA_BASIC_TO_IL_BATCH_CASES
                    "compare\t${case_name}\t${case_bas}\t${case_golden}\t\t\n")
        elseif ("${case_kind}" STREQUAL "bounds")
            string(APPEND _ZANNA_BASIC_TO_IL_BATCH_CASES
                    "bounds\t${case_name}\t${case_bas}\t${case_golden}\t\t\n")
        elseif ("${case_kind}" STREQUAL "contains")
            string(APPEND _ZANNA_BASIC_TO_IL_BATCH_CASES
                    "contains\t${case_name}\t${case_bas}\t\t${case_must_have}\t${case_must_not_have}\n")
        else ()
            message(FATAL_ERROR "Unknown BASIC-to-IL golden batch case kind '${case_kind}' for ${case_name}")
        endif ()
    endmacro()

    macro(_zanna_basic_to_il_add_ctest name)
        set(_case_bas "")
        set(_case_golden "")
        set(_case_must_have "")
        set(_case_must_not_have "")
        set(_case_kind "")
        set(_collect_field "")
        foreach (_arg ${ARGN})
            if ("${_arg}" MATCHES "^-D[A-Za-z_]+=" OR "${_arg}" STREQUAL "-P" OR "${_arg}" MATCHES "\\.cmake$")
                set(_collect_field "")
            endif ()

            if ("${_arg}" MATCHES "^-DBAS_FILE=(.*)$")
                set(_case_bas "${CMAKE_MATCH_1}")
            elseif ("${_arg}" MATCHES "^-DGOLDEN=(.*)$")
                set(_case_golden "${CMAKE_MATCH_1}")
            elseif ("${_arg}" MATCHES "^-DMUST_HAVE=(.*)$")
                set(_case_must_have "${CMAKE_MATCH_1}")
                set(_collect_field "must_have")
            elseif ("${_arg}" MATCHES "^-DMUST_NOT_HAVE=(.*)$")
                set(_case_must_not_have "${CMAKE_MATCH_1}")
                set(_collect_field "must_not_have")
            elseif ("${_arg}" STREQUAL "${_ZANNA_BASIC_TO_IL_CHECK}")
                set(_case_kind "compare")
            elseif ("${_arg}" STREQUAL "${_ZANNA_BASIC_TO_IL_CHECK_BOUNDS}")
                set(_case_kind "bounds")
            elseif ("${_arg}" STREQUAL "${_ZANNA_BASIC_TO_IL_CHECK_CONTAINS}")
                set(_case_kind "contains")
            elseif (_collect_field STREQUAL "must_have")
                string(APPEND _case_must_have ";${_arg}")
            elseif (_collect_field STREQUAL "must_not_have")
                string(APPEND _case_must_not_have ";${_arg}")
            endif ()
        endforeach ()

        if ("${_case_kind}" STREQUAL "" OR "${_case_bas}" STREQUAL "")
            message(FATAL_ERROR "Unable to batch BASIC-to-IL golden test ${name}")
        endif ()
        _zanna_basic_to_il_add_batch_case(
                "${name}" "${_case_kind}" "${_case_bas}" "${_case_golden}" "${_case_must_have}" "${_case_must_not_have}")

        if (ZANNA_ENABLE_INDIVIDUAL_BASIC_TO_IL_GOLDEN_TESTS)
            zanna_add_ctest(${name} ${ARGN})
        endif ()
    endmacro()

    _zanna_basic_to_il_add_ctest(basic_to_il_ex1
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/examples/zbasic/ex1_hello_cond.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/ex1_hello_cond.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_ex2
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/examples/zbasic/ex2_sum_1_to_10.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/ex2_sum_1_to_10.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_ex3
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/examples/zbasic/ex3_for_table.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/ex3_for_table.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_ex4
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/examples/zbasic/ex4_if_elseif.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/ex4_if_elseif.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_ex_elseif
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/examples/zbasic/ex_elseif.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/ex_elseif.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_ex6
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/examples/zbasic/ex6_array_sum.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/ex6_array_sum.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_array_dim_redim
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_TO_IL_DIR}/array_dim_redim.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/array_dim_redim.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_array_dim_runtime
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_TO_IL_DIR}/array_dim_runtime.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/array_dim_runtime.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_ubound
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_TO_IL_DIR}/ubound.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/ubound.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_not
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/examples/zbasic/ex_not.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/not.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_print_commas
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/examples/zbasic/ex_print_commas.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/print_commas.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_print_semicolons
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/examples/zbasic/ex_print_semicolons.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/print_semicolons.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_print_newline_control
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/examples/zbasic/ex_print_newline_control.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/print_newline_control.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_colon
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/examples/zbasic/ex_colon.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/ex_colon.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_loc
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_TO_IL_DIR}/loc_add.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/loc_add.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_strings
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_TO_IL_DIR}/strings.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/strings.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    # Ensure STRING arrays lower to the rt_arr_str_* family only (no i32 helpers)
    _zanna_basic_to_il_add_ctest(basic_to_il_string_arrays_contains
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_TO_IL_DIR}/string_array_contains.bas
            "-DMUST_HAVE=rt_arr_str_alloc;rt_arr_str_len;rt_arr_str_put;rt_arr_str_get"
            "-DMUST_NOT_HAVE=rt_arr_i32_"
            -P ${_ZANNA_BASIC_TO_IL_DIR}/check_il_contains.cmake)

    _zanna_basic_to_il_add_ctest(basic_to_il_string_cmp
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_TO_IL_DIR}/string_cmp.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/string_cmp.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    # Property access on Zanna.String lowers to canonical getter
    _zanna_basic_to_il_add_ctest(basic_to_il_string_prop_len
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_TO_IL_DIR}/string_prop_len.bas
            "-DMUST_HAVE=Zanna.String.get_Length"
            -P ${_ZANNA_BASIC_TO_IL_DIR}/check_il_contains.cmake)

    # Method call on Zanna.String lowers with receiver-as-arg0
    _zanna_basic_to_il_add_ctest(basic_to_il_string_method_substring
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_TO_IL_DIR}/string_method_substring.bas
            "-DMUST_HAVE=Zanna.String.Substring"
            -P ${_ZANNA_BASIC_TO_IL_DIR}/check_il_contains.cmake)

    # NEW Zanna.String maps to ctor helper when present
    _zanna_basic_to_il_add_ctest(basic_to_il_string_new_ctor
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_TO_IL_DIR}/string_new_ctor.bas
            "-DMUST_HAVE=Zanna.String.Chr"
            -P ${_ZANNA_BASIC_TO_IL_DIR}/check_il_contains.cmake)

    _zanna_basic_to_il_add_ctest(basic_to_il_boolean_literals
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_GOLDEN_SRC_DIR}/boolean_literals.bas
            -DGOLDEN=${_ZANNA_GOLDEN_IL_DIR}/boolean_literals.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_boolean_dim_and_assign
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_GOLDEN_SRC_DIR}/boolean_dim_and_assign.bas
            -DGOLDEN=${_ZANNA_GOLDEN_IL_DIR}/boolean_dim_and_assign.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_boolean_branch_join_skeleton
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_GOLDEN_SRC_DIR}/boolean_branch_join_skeleton.bas
            -DGOLDEN=${_ZANNA_GOLDEN_IL_DIR}/boolean_branch_join_skeleton.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_boolean_not
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_GOLDEN_SRC_DIR}/boolean_not.bas
            -DGOLDEN=${_ZANNA_GOLDEN_IL_DIR}/boolean_not.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_boolean_and
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_GOLDEN_SRC_DIR}/boolean_and.bas
            -DGOLDEN=${_ZANNA_GOLDEN_IL_DIR}/boolean_and.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_boolean_or
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_GOLDEN_SRC_DIR}/boolean_or.bas
            -DGOLDEN=${_ZANNA_GOLDEN_IL_DIR}/boolean_or.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    # Skip on Windows: short-circuit boolean tests cause crashes (to be investigated)
    if (NOT WIN32)
        _zanna_basic_to_il_add_ctest(basic_to_il_boolean_andalso
                ${CMAKE_COMMAND}
                -DILC=${_ZANNA_BASIC_ILC}
                -DBAS_FILE=${_ZANNA_GOLDEN_SRC_DIR}/boolean_andalso.bas
                -DGOLDEN=${_ZANNA_GOLDEN_IL_DIR}/boolean_andalso.il
                -P ${_ZANNA_BASIC_TO_IL_CHECK})

        _zanna_basic_to_il_add_ctest(basic_to_il_boolean_orelse
                ${CMAKE_COMMAND}
                -DILC=${_ZANNA_BASIC_ILC}
                -DBAS_FILE=${_ZANNA_GOLDEN_SRC_DIR}/boolean_orelse.bas
                -DGOLDEN=${_ZANNA_GOLDEN_IL_DIR}/boolean_orelse.il
                -P ${_ZANNA_BASIC_TO_IL_CHECK})
    endif ()

    _zanna_basic_to_il_add_ctest(basic_to_il_conversions
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_TO_IL_DIR}/conversions.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/conversions.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_float_ops
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_TO_IL_DIR}/float_ops.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/float_ops.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_math_phase1
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_TO_IL_DIR}/math_phase1.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/math_phase1.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_math_phase2
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_TO_IL_DIR}/math_phase2.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/math_phase2.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_abs_mixed
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_TO_IL_DIR}/abs_mixed.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/abs_mixed.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_bounds_checks
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_TO_IL_DIR}/bounds_check.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/bounds_check.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK_BOUNDS})

    _zanna_basic_to_il_add_ctest(basic_to_il_randomize
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_TO_IL_DIR}/randomize.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/randomize.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_rnd
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_TO_IL_DIR}/rnd.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/rnd.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_lower_funcdef_only
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_TO_IL_DIR}/lower_funcdef_only.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/lower_funcdef_only.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_calls_lowering
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_TO_IL_DIR}/calls_lowering.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/calls_lowering.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_instr
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_TO_IL_DIR}/instr.bas
            -DGOLDEN=${_ZANNA_BASIC_TO_IL_DIR}/instr.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_len_mid
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_GOLDENS_DIR}/LenMid.bas
            -DGOLDEN=${_ZANNA_BASIC_GOLDENS_DIR}/LenMid.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_instr_dispatch
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_GOLDENS_DIR}/Instr.bas
            -DGOLDEN=${_ZANNA_BASIC_GOLDENS_DIR}/Instr.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_labels_proc_loops
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_GOLDENS_DIR}/labels_proc_loops.bas
            -DGOLDEN=${_ZANNA_BASIC_GOLDENS_DIR}/labels_proc_loops.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_suffix_free_vars
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_GOLDENS_DIR}/SuffixFreeVars.bas
            -DGOLDEN=${_ZANNA_BASIC_GOLDENS_DIR}/SuffixFreeVars.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_lower_program_with_proc
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_GOLDENS_DIR}/LowerProgramWithProc.bas
            -DGOLDEN=${_ZANNA_BASIC_GOLDENS_DIR}/LowerProgramWithProc.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    _zanna_basic_to_il_add_ctest(basic_to_il_on_error_push_pop
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_EH_LOWERING_DIR}/on_error_push_pop.bas
            -DGOLDEN=${_ZANNA_EH_LOWERING_DIR}/on_error_push_pop.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    # Skip on Windows: resume_forms compilation crashes
    if (NOT WIN32)
        _zanna_basic_to_il_add_ctest(basic_to_il_resume_forms
                ${CMAKE_COMMAND}
                -DILC=${_ZANNA_BASIC_ILC}
                -DBAS_FILE=${_ZANNA_EH_LOWERING_DIR}/resume_forms.bas
                -DGOLDEN=${_ZANNA_EH_LOWERING_DIR}/resume_forms.il
                -P ${_ZANNA_BASIC_TO_IL_CHECK})
    endif ()

    _zanna_basic_to_il_add_ctest(basic_to_il_return_nest
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_GOLDENS_DIR}/ReturnNest.bas
            -DGOLDEN=${_ZANNA_BASIC_GOLDENS_DIR}/ReturnNest.il
            -P ${_ZANNA_BASIC_TO_IL_CHECK})

    zanna_add_test(test_basic_to_il_goldens_batch
            SRCS ${_ZANNA_BASIC_TO_IL_DIR}/BasicToIlBatchRunner.cpp
            LIBS ${ZANNA_BASIC_LIBS} zanna::il_io
            NO_CTEST)

    set(_ZANNA_BASIC_TO_IL_BATCH_CASES_FILE
            "${CMAKE_CURRENT_BINARY_DIR}/basic_to_il_goldens_batch_cases.tsv")
    file(GENERATE
            OUTPUT "${_ZANNA_BASIC_TO_IL_BATCH_CASES_FILE}"
            CONTENT "${_ZANNA_BASIC_TO_IL_BATCH_CASES}")
    math(EXPR _ZANNA_BASIC_TO_IL_BATCH_LAST_SHARD "${ZANNA_BASIC_TO_IL_GOLDEN_BATCH_SHARDS} - 1")
    foreach (_ZANNA_BASIC_TO_IL_BATCH_SHARD RANGE 0 ${_ZANNA_BASIC_TO_IL_BATCH_LAST_SHARD})
        zanna_add_ctest(basic_to_il_goldens_batch_${_ZANNA_BASIC_TO_IL_BATCH_SHARD}
                test_basic_to_il_goldens_batch
                ${_ZANNA_BASIC_TO_IL_BATCH_CASES_FILE}
                ${_ZANNA_BASIC_TO_IL_BATCH_SHARD}
                ${ZANNA_BASIC_TO_IL_GOLDEN_BATCH_SHARDS})
        zanna_add_test_labels(basic_to_il_goldens_batch_${_ZANNA_BASIC_TO_IL_BATCH_SHARD} basic)
    endforeach ()

    # Guards the -DUPDATE_GOLDEN=1 / UPDATE_GOLDEN=1 refresh path in every
    # BASIC-to-IL golden harness so scripts/update_goldens.sh can repair extern
    # drift instead of leaving the goldens to be hand-edited.
    zanna_add_ctest(basic_to_il_golden_update_path
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBATCH_RUNNER=$<TARGET_FILE:test_basic_to_il_goldens_batch>
            -DCHECK_IL=${_ZANNA_BASIC_TO_IL_CHECK}
            -DCHECK_IL_BOUNDS=${_ZANNA_BASIC_TO_IL_CHECK_BOUNDS}
            -DBAS_FILE=${_ZANNA_BASIC_TO_IL_DIR}/gosub.bas
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/basic_to_il_golden_update_path
            -P ${_ZANNA_BASIC_TO_IL_DIR}/check_golden_update.cmake)
    zanna_add_test_labels(basic_to_il_golden_update_path basic)
endfunction()

function(zanna_add_basic_strings_test)
    if (TEST basic_strings)
        return()
    endif ()
    # Shell script test - skip on Windows
    if (WIN32)
        return()
    endif ()

    zanna_add_ctest(basic_strings ${CMAKE_SOURCE_DIR}/scripts/test_strings.sh ${CMAKE_BINARY_DIR})
    zanna_set_test_labels(basic_strings basic requires_posix_shell)
endfunction()

function(zanna_add_basic_semantics_golden_tests)
    if (TEST basic_semantics_proc_registration)
        return()
    endif ()

    set(_ZANNA_BASIC_ILC $<TARGET_FILE:zanna>)
    set(_ZANNA_BASIC_SEMANTICS_DIR ${ZANNA_TESTS_DIR}/basic/semantics)
    set(_ZANNA_BASIC_NEGATIVES_DIR ${ZANNA_TESTS_DIR}/basic/negatives)
    set(_ZANNA_GOLDEN_ERRORS_DIR ${ZANNA_TESTS_DIR}/golden/basic_errors)

    file(READ ${_ZANNA_BASIC_SEMANTICS_DIR}/proc_registration_ok.err EXPECT_PROC_REG)
    string(STRIP "${EXPECT_PROC_REG}" EXPECT_PROC_REG)
    zanna_add_ctest(basic_semantics_proc_registration
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/proc_registration_ok.bas
            "-DEXPECT=${EXPECT_PROC_REG}"
            -P ${_ZANNA_GOLDEN_ERRORS_DIR}/check_error.cmake)

    file(READ ${_ZANNA_BASIC_SEMANTICS_DIR}/missing_return.err EXPECT_MISSING_RET)
    string(STRIP "${EXPECT_MISSING_RET}" EXPECT_MISSING_RET)
    zanna_add_ctest(basic_semantics_missing_return
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/missing_return.bas
            "-DEXPECT=${EXPECT_MISSING_RET}"
            -P ${_ZANNA_GOLDEN_ERRORS_DIR}/check_error.cmake)

    file(READ ${_ZANNA_BASIC_SEMANTICS_DIR}/sub_in_expr.err EXPECT_SUB_IN_EXPR)
    string(STRIP "${EXPECT_SUB_IN_EXPR}" EXPECT_SUB_IN_EXPR)
    zanna_add_ctest(basic_semantics_sub_in_expr
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/sub_in_expr.bas
            "-DEXPECT=${EXPECT_SUB_IN_EXPR}"
            -P ${_ZANNA_GOLDEN_ERRORS_DIR}/check_error.cmake)

    zanna_add_ctest(basic_semantics_funcs_then_code
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/FuncsThenCode.ok.bas
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_ok.cmake)

    zanna_add_ctest(basic_semantics_boolean_type_ok
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/test_boolean_type_ok.bas
            -DDIAG_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/test_boolean_type_ok.diag
            -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_diag.cmake)

    zanna_add_ctest(basic_semantics_condition_boolean_ok
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/test_condition_boolean_ok.bas
            -DDIAG_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/test_condition_boolean_ok.diag
            -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_diag.cmake)

    zanna_add_ctest(basic_semantics_condition_nonboolean_errors
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/test_condition_nonboolean_errors.bas
            -DDIAG_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/test_condition_nonboolean_errors.diag
            -DEXPECT_STATUS=1
            -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_diag.cmake)

    zanna_add_ctest(basic_semantics_color_locate_type_errors
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/test_color_locate_type_errors.bas
            -DDIAG_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/test_color_locate_type_errors.diag
            -DEXPECT_STATUS=1
            -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_diag.cmake)

    zanna_add_ctest(basic_negatives_if_int_condition
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_NEGATIVES_DIR}/test_if_int_condition.bas
            -DDIAG_FILE=${_ZANNA_BASIC_NEGATIVES_DIR}/test_if_int_condition.diag
            -DEXPECT_STATUS=0
            -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_diag.cmake)

    zanna_add_ctest(basic_negatives_while_float_condition
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_NEGATIVES_DIR}/test_while_float_condition.bas
            -DDIAG_FILE=${_ZANNA_BASIC_NEGATIVES_DIR}/test_while_float_condition.diag
            -DEXPECT_STATUS=1
            -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_diag.cmake)

    zanna_add_ctest(basic_semantics_boolean_constfold
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/test_boolean_constfold.bas
            -DEXPECT_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/test_boolean_constfold.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    zanna_add_ctest(basic_semantics_ident_with_underscore
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/ident_with_underscore.bas
            -DEXPECT_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/ident_with_underscore.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    zanna_add_ctest(basic_semantics_unary_numeric
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/test_unary_numeric.bas
            -DEXPECT_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/test_unary_numeric.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    zanna_add_ctest(basic_semantics_arith_mix
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/arith_mix.bas
            -DEXPECT_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/arith_mix.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    zanna_add_ctest(basic_semantics_divide_by_zero
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/divide_by_zero.bas
            -DDIAG_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/divide_by_zero.diag
            -DEXPECT_STATUS=1
            -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_diag.cmake)

    # Multi-dimensional array bounds checking tests
    zanna_add_ctest(basic_semantics_array_multidim_bounds
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/array_multidim_bounds.bas
            -DDIAG_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/array_multidim_bounds.diag
            -DEXPECT_STATUS=1
            -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_diag.cmake)

    zanna_add_ctest(basic_semantics_array_multidim_ok
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/array_multidim_ok.bas
            -DDIAG_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/array_multidim_ok.diag
            -DEXPECT_STATUS=0
            -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_diag.cmake)

    set(_ZANNA_UNKNOWN_STMT_REGEX "unknown statement 'FROB'")
    zanna_add_ctest(basic_semantics_unknown_statement
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/unknown_statement.bas
            "-DEXPECT=${_ZANNA_UNKNOWN_STMT_REGEX}"
            -P ${_ZANNA_GOLDEN_ERRORS_DIR}/check_error.cmake)

    zanna_add_ctest(basic_semantics_gosub_ok
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/gosub_ok.bas
            -DEXPECT_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/gosub_ok.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    # Skip on Windows: GOSUB runtime error handling crashes
    if (NOT WIN32)
        zanna_add_ctest(basic_semantics_gosub_empty_return
                ${CMAKE_COMMAND}
                -DILC=${_ZANNA_BASIC_ILC}
                -DBAS_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/gosub_empty_return.bas
                "-DEXPECT=gosub: empty return stack"
                -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_run_error.cmake)
    endif ()

    zanna_add_ctest(basic_semantics_input_single_diag
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/input_single_diag.bas
            -DSTDIN_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/input_single_diag.stdin
            "-DEXPECT=INPUT: expected 2 values, got 1"
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_run_error.cmake)

    zanna_add_ctest(basic_semantics_input_multi_ok
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/input_single_diag.bas
            -DSTDIN_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/input_multi_ok.stdin
            -DEXPECT_FILE=${_ZANNA_BASIC_SEMANTICS_DIR}/input_multi_ok.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)
endfunction()

function(zanna_add_basic_proc_tests)
    if (TEST basic_proc_forward_calls)
        return()
    endif ()

    set(_ZANNA_BASIC_ILC $<TARGET_FILE:zanna>)
    set(_ZANNA_BASIC_PROCS_DIR ${ZANNA_TESTS_DIR}/basic/procs)
    set(_ZANNA_BASIC_SEMANTICS_DIR ${ZANNA_TESTS_DIR}/basic/semantics)

    zanna_add_ctest(basic_proc_forward_calls
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_PROCS_DIR}/forward_calls.bas
            -DEXPECT_FILE=${_ZANNA_BASIC_PROCS_DIR}/forward_calls.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)
endfunction()

function(zanna_add_basic_call_tests)
    if (TEST basic_call_stmt_sub)
        return()
    endif ()

    set(_ZANNA_BASIC_ILC $<TARGET_FILE:zanna>)
    set(_ZANNA_BASIC_CALLS_DIR ${ZANNA_TESTS_DIR}/basic/calls)
    set(_ZANNA_BASIC_SEMANTICS_DIR ${ZANNA_TESTS_DIR}/basic/semantics)
    set(_ZANNA_GOLDEN_ERRORS_DIR ${ZANNA_TESTS_DIR}/golden/basic_errors)

    zanna_add_ctest(basic_call_stmt_sub
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_CALLS_DIR}/call_stmt_sub.bas
            -DEXPECT_FILE=${_ZANNA_BASIC_CALLS_DIR}/call_stmt_sub.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    zanna_add_ctest(basic_call_stmt_sub_with_builtin
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_CALLS_DIR}/call_stmt_sub_with_builtin.bas
            -DEXPECT_FILE=${_ZANNA_BASIC_CALLS_DIR}/call_stmt_sub_with_builtin.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    file(READ ${_ZANNA_BASIC_CALLS_DIR}/bare_call_error.err EXPECT_BARE_CALL_ERROR)
    string(STRIP "${EXPECT_BARE_CALL_ERROR}" EXPECT_BARE_CALL_ERROR)

    zanna_add_ctest(basic_call_bare_call_error
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_CALLS_DIR}/bare_call_error.bas
            "-DEXPECT=${EXPECT_BARE_CALL_ERROR}"
            -P ${_ZANNA_GOLDEN_ERRORS_DIR}/check_error.cmake)
endfunction()

function(zanna_add_basic_integration_tests)
    if (TEST basic_boolean_andalso_short)
        return()
    endif ()

    set(_ZANNA_BASIC_ILC $<TARGET_FILE:zanna>)
    set(_ZANNA_BASIC_INTEGRATION_DIR ${ZANNA_TESTS_DIR}/integration)
    set(_ZANNA_BASIC_SEMANTICS_DIR ${ZANNA_TESTS_DIR}/basic/semantics)
    set(_ZANNA_GOLDEN_ERRORS_DIR ${ZANNA_TESTS_DIR}/golden/basic_errors)

    zanna_add_ctest(basic_boolean_andalso_short
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_INTEGRATION_DIR}/basic_boolean_andalso_short.bas
            -DEXPECT_FILE=${_ZANNA_BASIC_INTEGRATION_DIR}/basic_boolean_andalso_short.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    zanna_add_ctest(basic_boolean_truth
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_INTEGRATION_DIR}/basic_boolean_truth.bas
            -DEXPECT_FILE=${_ZANNA_BASIC_INTEGRATION_DIR}/basic_boolean_truth.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    zanna_add_ctest(basic_boolean_shortcircuit
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_INTEGRATION_DIR}/basic_shortcircuit.bas
            -DEXPECT_FILE=${_ZANNA_BASIC_INTEGRATION_DIR}/basic_shortcircuit.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    # Skip on Windows: GOSUB compilation crashes
    if (NOT WIN32)
        zanna_add_ctest(basic_gosub_label_only
                ${CMAKE_COMMAND}
                -DILC=${_ZANNA_BASIC_ILC}
                -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/gosub_label_only.bas
                -DEXPECT_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/gosub_label_only.out
                -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)
    endif ()

    # Regression: module-level globals visible to SUB/FUNCTION (BUG-030)
    zanna_add_ctest(basic_globals_sub_mod
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/semantics/globals_sub_mod.bas
            -DEXPECT_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/semantics/globals_sub_mod.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    # BUG-057: BOOLEAN return type in class methods
    zanna_add_ctest(basic_oop_boolean_method_return
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_INTEGRATION_DIR}/basic_oop_boolean_method_return.bas
            -DEXPECT_FILE=${_ZANNA_BASIC_INTEGRATION_DIR}/basic_oop_boolean_method_return.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    # Regression: object parameters are borrowed, including objects with array fields.
    zanna_add_ctest(basic_oop_object_param_borrowed
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_INTEGRATION_DIR}/basic_oop_object_param_borrowed.bas
            -DEXPECT_FILE=${_ZANNA_BASIC_INTEGRATION_DIR}/basic_oop_object_param_borrowed.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    # BUG-094: 2D integer array field in class stores distinct values across columns (dotted access)
    # zanna_add_ctest(basic_oop_int_array2d_field
    #         ${CMAKE_COMMAND}
    #         -DILC=${_ZANNA_BASIC_ILC}
    #         -DBAS_FILE=${_ZANNA_BASIC_INTEGRATION_DIR}/basic_oop_int_array2d_field.bas
    #         -DEXPECT_FILE=${_ZANNA_BASIC_INTEGRATION_DIR}/basic_oop_int_array2d_field.expect
    #         -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    # BUG-094: 2D integer array field in class (implicit field access inside method)
    # zanna_add_ctest(basic_oop_int_array2d_field_implicit
    #         ${CMAKE_COMMAND}
    #         -DILC=${_ZANNA_BASIC_ILC}
    #         -DBAS_FILE=${_ZANNA_BASIC_INTEGRATION_DIR}/basic_oop_int_array2d_field_implicit.bas
    #         -DEXPECT_FILE=${_ZANNA_BASIC_INTEGRATION_DIR}/basic_oop_int_array2d_field_implicit.expect
    #         -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    # BUG-058: String array fields persist values —
    # Test intentionally not registered until bug is fixed to keep CI green.

    # BUG-019 regression: i1 boolean returns in native codegen
    zanna_add_ctest(basic_regress_bug019_bool_native
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/regress_bug019_bool_native.bas
            -DEXPECT_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/regress_bug019_bool_native.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    # BUG-016 regression: BASIC lexer "" escaped quotes
    zanna_add_ctest(basic_regress_bug016_escaped_quotes
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/regress_bug016_escaped_quotes.bas
            -DEXPECT_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/regress_bug016_escaped_quotes.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    # BUG-003 regression: NOT operator always returns BOOLEAN
    zanna_add_ctest(basic_regress_bug003_not_bool
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/regress_bug003_not_bool.bas
            -DEXPECT_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/regress_bug003_not_bool.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    # BUG-017 regression: SafeI64 works on Windows
    zanna_add_ctest(basic_regress_bug017_safe_i64
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/regress_bug017_safe_i64.bas
            -DEXPECT_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/regress_bug017_safe_i64.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    zanna_add_ctest(basic_regress_bug001_print_ptr
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/regress_bug001_print_ptr.bas
            -DEXPECT_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/regress_bug001_print_ptr.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    zanna_add_ctest(basic_regress_bug023_str_obj_coerce
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/regress_bug023_str_obj_coerce.bas
            -DEXPECT_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/regress_bug023_str_obj_coerce.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    zanna_add_ctest(basic_regress_bug011_random_new
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/regress_bug011_random_new.bas
            -DEXPECT_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/regress_bug011_random_new.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    zanna_add_ctest(basic_regress_bug005_str_convert
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/regress_bug005_str_convert.bas
            -DEXPECT_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/regress_bug005_str_convert.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    zanna_add_ctest(basic_regress_fileio_write_ok
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/regress_fileio_write_ok.bas
            -DEXPECT_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/regress_fileio_write_ok.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    zanna_add_ctest(basic_regress_me_keyword
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/regress_me_keyword.bas
            -DEXPECT_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/regress_me_keyword.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    zanna_add_ctest(basic_select_case_relop
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/select_case_relop.bas
            -DEXPECT_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/select_case_relop.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    file(READ ${CMAKE_SOURCE_DIR}/src/tests/basic/select_case_mixed_types.err EXPECT_SELECT_CASE_MIXED_TYPES)
    string(STRIP "${EXPECT_SELECT_CASE_MIXED_TYPES}" EXPECT_SELECT_CASE_MIXED_TYPES)

    zanna_add_ctest(basic_select_case_mixed_types
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/select_case_mixed_types.bas
            "-DEXPECT=${EXPECT_SELECT_CASE_MIXED_TYPES}"
            -P ${_ZANNA_GOLDEN_ERRORS_DIR}/check_error.cmake)

    file(READ ${CMAKE_SOURCE_DIR}/src/tests/basic/select_case_double_else.err EXPECT_SELECT_CASE_DOUBLE_ELSE)
    string(STRIP "${EXPECT_SELECT_CASE_DOUBLE_ELSE}" EXPECT_SELECT_CASE_DOUBLE_ELSE)

    zanna_add_ctest(basic_select_case_double_else
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${CMAKE_SOURCE_DIR}/src/tests/basic/select_case_double_else.bas
            "-DEXPECT=${EXPECT_SELECT_CASE_DOUBLE_ELSE}"
            -P ${_ZANNA_GOLDEN_ERRORS_DIR}/check_error.cmake)
endfunction()

function(zanna_add_basic_namespace_tests)
    if (TEST basic_ns_t01_flatten)
        return()
    endif ()

    set(_ZANNA_BASIC_ILC $<TARGET_FILE:zanna>)
    set(_ZANNA_BASIC_NS_DIR ${ZANNA_TESTS_DIR}/basic/namespaces)
    set(_ZANNA_BASIC_SEMANTICS_DIR ${ZANNA_TESTS_DIR}/basic/semantics)
    set(_ZANNA_GOLDEN_ERRORS_DIR ${ZANNA_TESTS_DIR}/golden/basic_errors)

    # t01: Flattened namespace call
    zanna_add_ctest(basic_ns_t01_flatten
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_NS_DIR}/t01_flatten.bas
            -DEXPECT_FILE=${_ZANNA_BASIC_NS_DIR}/t01_flatten.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    # t02: Nested namespace call
    zanna_add_ctest(basic_ns_t02_nested
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_NS_DIR}/t02_nested.bas
            -DEXPECT_FILE=${_ZANNA_BASIC_NS_DIR}/t02_nested.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    # t03: Parent-walk resolution within nested namespace
    zanna_add_ctest(basic_ns_t03_parent_walk
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_NS_DIR}/t03_parent_walk.bas
            -DEXPECT_FILE=${_ZANNA_BASIC_NS_DIR}/t03_parent_walk.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    # t04_1: Cross-file defines only (no output)
    zanna_add_ctest(basic_ns_t04_cross_file_1
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_NS_DIR}/t04_cross_file_1.bas
            -DEXPECT_FILE=${_ZANNA_BASIC_NS_DIR}/t04_cross_file_1.expect
            -P ${_ZANNA_BASIC_SEMANTICS_DIR}/check_expect.cmake)

    # t04_2: Cross-file usage without imports should fail with unknown variable 'LIB'
    # (BUG-120 fix changed error from 'unknown procedure' to 'unknown variable' for method
    # calls on undefined base variables - the new message is more accurate)
    zanna_add_ctest(basic_ns_t04_cross_file_2_fail
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_NS_DIR}/t04_cross_file_2.bas
            "-DEXPECT=unknown variable 'LIB'"
            -P ${_ZANNA_GOLDEN_ERRORS_DIR}/check_error.cmake)

    # t05: Unqualified unknown with attempted candidates listed
    zanna_add_ctest(basic_ns_t05_unknown_parent_walk
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_ZANNA_BASIC_NS_DIR}/t05_unknown_parent_walk.bas
            "-DEXPECT=unknown procedure 'f' \\(tried: a\\.b\\.f, a\\.f, f\\)"
            -P ${_ZANNA_GOLDEN_ERRORS_DIR}/check_error.cmake)

    # Phase 2 tests ----------------------------------------------------------
    set(_P2_DIR ${ZANNA_TESTS_DIR}/basic/namespaces_phase2)
    set(_SEM_DIR ${ZANNA_TESTS_DIR}/basic/semantics)

    # Spec-compliant: USING after declarations is an error (E_NS_005)
    zanna_add_ctest(basic_ns2_t01_import_proc
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_P2_DIR}/t01_using_import_proc.bas
            "-DEXPECT=E_NS_005"
            -P ${_ZANNA_GOLDEN_ERRORS_DIR}/check_error.cmake)

    # Spec-compliant: USING after declarations is an error (E_NS_005)
    zanna_add_ctest(basic_ns2_t02_alias_qualified_call
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_P2_DIR}/t02_using_alias_qualified_call.bas
            "-DEXPECT=E_NS_005"
            -P ${_ZANNA_GOLDEN_ERRORS_DIR}/check_error.cmake)

    # Spec-compliant: USING inside namespace is an error (E_NS_008)
    zanna_add_ctest(basic_ns2_t03_shadowing_local_vs_import
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_P2_DIR}/t03_shadowing_local_vs_import.bas
            "-DEXPECT=E_NS_008"
            -P ${_ZANNA_GOLDEN_ERRORS_DIR}/check_error.cmake)

    zanna_add_ctest(basic_ns2_t04_ambiguity_imports
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_P2_DIR}/t04_ambiguous_imports.bas
            "-DEXPECT=B2009"
            -P ${_ZANNA_GOLDEN_ERRORS_DIR}/check_error.cmake)

    # Spec-compliant: USING after declarations is an error (E_NS_005)
    zanna_add_ctest(basic_ns2_t05_types_with_imports
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_P2_DIR}/t05_types_with_imports.bas
            "-DEXPECT=E_NS_005"
            -P ${_ZANNA_GOLDEN_ERRORS_DIR}/check_error.cmake)

    zanna_add_ctest(basic_ns2_t06_alias_does_not_open
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_P2_DIR}/t06_alias_does_not_open.bas
            "-DEXPECT=B1006"
            -P ${_ZANNA_GOLDEN_ERRORS_DIR}/check_error.cmake)

    # Spec-compliant: USING after declarations is an error (E_NS_005)
    zanna_add_ctest(basic_ns2_t07_case_insensitive
            ${CMAKE_COMMAND}
            -DILC=${_ZANNA_BASIC_ILC}
            -DBAS_FILE=${_P2_DIR}/t07_case_insensitive.bas
            "-DEXPECT=E_NS_005"
            -P ${_ZANNA_GOLDEN_ERRORS_DIR}/check_error.cmake)
endfunction()
