## SPDX-License-Identifier: GPL-3.0-only
## File: tests/e2e/test_front_basic.cmake
## Purpose: Exercise BASIC front end via simple programs.
## Key invariants: Program output and emitted IL match expectations.
## Ownership/Lifetime: Invoked by CTest.
## Links: docs/examples.md

execute_process(COMMAND ${ILC} front basic -emit-il ${SRC_DIR}/examples/zbasic/ex1_hello_cond.bas
        OUTPUT_FILE basic.il RESULT_VARIABLE r1)
if (NOT r1 EQUAL 0)
    message(FATAL_ERROR "emit-il failed")
endif ()
file(READ basic.il IL_OUT)
string(REGEX MATCH "HELLO" _il1 "${IL_OUT}")
if (NOT _il1)
    message(FATAL_ERROR "missing HELLO in IL output")
endif ()

execute_process(COMMAND ${ILC} front basic -run ${SRC_DIR}/examples/zbasic/ex1_hello_cond.bas
        OUTPUT_FILE run.txt RESULT_VARIABLE r2)
if (NOT r2 EQUAL 0)
    message(FATAL_ERROR "execution failed")
endif ()
file(READ run.txt R1)
string(REGEX MATCH "HELLO" _m1 "${R1}")
if (NOT _m1)
    message(FATAL_ERROR "missing HELLO")
endif ()
string(REGEX MATCH "READY" _m2 "${R1}")
if (NOT _m2)
    message(FATAL_ERROR "missing READY")
endif ()
string(REGEX MATCHALL "[0-9]+" nums "${R1}")
list(LENGTH nums n)
if (NOT n EQUAL 2)
    message(FATAL_ERROR "expected two integers in output")
endif ()

execute_process(COMMAND ${ILC} front basic -run ${SRC_DIR}/examples/zbasic/ex2_sum_1_to_10.bas
        OUTPUT_FILE run2.txt RESULT_VARIABLE r3)
if (NOT r3 EQUAL 0)
    message(FATAL_ERROR "execution ex2 failed")
endif ()
file(READ run2.txt R2)
string(REGEX MATCH "SUM" _s1 "${R2}")
if (NOT _s1)
    message(FATAL_ERROR "missing SUM")
endif ()
string(REGEX MATCH "DONE" _s2 "${R2}")
if (NOT _s2)
    message(FATAL_ERROR "missing DONE")
endif ()
string(REGEX MATCH "45" _s3 "${R2}")
if (NOT _s3)
    message(FATAL_ERROR "missing 45")
endif ()

# test PRINT with commas
execute_process(COMMAND ${ILC} front basic -run ${SRC_DIR}/examples/zbasic/ex_print_commas.bas
        OUTPUT_FILE run_commas.txt RESULT_VARIABLE rc)
if (NOT rc EQUAL 0)
    message(FATAL_ERROR "execution print_commas failed")
endif ()
file(READ run_commas.txt RC)
if (NOT RC STREQUAL "A             1             B\n")
    message(FATAL_ERROR "unexpected print_commas output: ${RC}")
endif ()

# test PRINT with semicolons
execute_process(COMMAND ${ILC} front basic -run ${SRC_DIR}/examples/zbasic/ex_print_semicolons.bas
        OUTPUT_FILE run_semicolons.txt RESULT_VARIABLE rs)
if (NOT rs EQUAL 0)
    message(FATAL_ERROR "execution print_semicolons failed")
endif ()
file(READ run_semicolons.txt RS)
if (NOT RS STREQUAL "A1B\n")
    message(FATAL_ERROR "unexpected print_semicolons output: ${RS}")
endif ()

# test PRINT newline control with trailing ';'
execute_process(COMMAND ${ILC} front basic -run ${SRC_DIR}/examples/zbasic/ex_print_newline_control.bas
        OUTPUT_FILE run_nl.txt RESULT_VARIABLE rn)
if (NOT rn EQUAL 0)
    message(FATAL_ERROR "execution print_newline_control failed")
endif ()
file(READ run_nl.txt RN)
if (NOT RN STREQUAL "AB\n")
    message(FATAL_ERROR "unexpected print_newline_control output: ${RN}")
endif ()

# test INPUT string echo
execute_process(COMMAND ${ILC} front basic -run ${SRC_DIR}/examples/zbasic/ex5_input_echo.bas --stdin-from ${SRC_DIR}/src/tests/data/input1.txt
        OUTPUT_FILE run3.txt RESULT_VARIABLE r4)
if (NOT r4 EQUAL 0)
    message(FATAL_ERROR "execution ex5 failed")
endif ()
file(READ run3.txt R3)
string(REGEX MATCH "hello world" _e1 "${R3}")
if (NOT _e1)
    message(FATAL_ERROR "missing echoed input")
endif ()

# test integer INPUT addition
set(tmp_bas "${CMAKE_BINARY_DIR}/input_add.bas")
file(WRITE ${tmp_bas} "10 INPUT A\n20 INPUT B\n30 PRINT A + B\n")
set(tmp_in "${CMAKE_BINARY_DIR}/input_nums.txt")
file(WRITE ${tmp_in} "3\n4\n")
execute_process(COMMAND ${ILC} front basic -run ${tmp_bas} --stdin-from ${tmp_in}
        OUTPUT_FILE run4.txt RESULT_VARIABLE r5)
if (NOT r5 EQUAL 0)
    message(FATAL_ERROR "execution add failed")
endif ()
file(READ run4.txt R4)
string(REGEX MATCH "7" _n1 "${R4}")
if (NOT _n1)
    message(FATAL_ERROR "missing numeric sum")
endif ()

# test INPUT prompt literal
execute_process(COMMAND ${ILC} front basic -run ${SRC_DIR}/examples/zbasic/ex_input_prompt_min.bas --stdin-from ${SRC_DIR}/src/tests/data/n_input.txt
        OUTPUT_FILE run_prompt.txt RESULT_VARIABLE rp)
if (NOT rp EQUAL 0)
    message(FATAL_ERROR "execution input_prompt failed")
endif ()
file(READ run_prompt.txt RP)
if (NOT RP STREQUAL "N=42\n")
    message(FATAL_ERROR "unexpected input_prompt output: ${RP}")
endif ()

# test array DIM and element access
set(tmp_arr_in "${CMAKE_BINARY_DIR}/array_n.txt")
file(WRITE ${tmp_arr_in} "5\n")
execute_process(COMMAND ${ILC} front basic -run ${SRC_DIR}/examples/zbasic/ex6_array_sum.bas --stdin-from ${tmp_arr_in}
        OUTPUT_FILE run5.txt RESULT_VARIABLE r6)
if (NOT r6 EQUAL 0)
    message(FATAL_ERROR "execution ex6 failed")
endif ()
file(READ run5.txt R5)
string(REGEX MATCH "30" _n2 "${R5}")
if (NOT _n2)
    message(FATAL_ERROR "missing array sum")
endif ()

# test multi-statement lines with ':'
execute_process(COMMAND ${ILC} front basic -run ${SRC_DIR}/examples/zbasic/ex_colon.bas
        OUTPUT_FILE run_colon.txt RESULT_VARIABLE rcol)
if (NOT rcol EQUAL 0)
    message(FATAL_ERROR "execution ex_colon failed")
endif ()
file(READ run_colon.txt RCOL)
if (NOT RCOL STREQUAL "1\n2\n")
    message(FATAL_ERROR "unexpected ex_colon output: ${RCOL}")
endif ()

# test unary NOT
execute_process(COMMAND ${ILC} front basic -run ${SRC_DIR}/examples/zbasic/ex_not.bas
        OUTPUT_FILE run_not.txt RESULT_VARIABLE rn)
if (NOT rn EQUAL 0)
    message(FATAL_ERROR "execution ex_not failed")
endif ()
file(READ run_not.txt Rn)
if (NOT Rn STREQUAL "1\n")
    message(FATAL_ERROR "unexpected ex_not output: ${Rn}")
endif ()

# test ELSEIF chain
execute_process(COMMAND ${ILC} front basic -run ${SRC_DIR}/examples/zbasic/ex_elseif.bas
        OUTPUT_FILE run_elseif.txt RESULT_VARIABLE relief)
if (NOT relief EQUAL 0)
    message(FATAL_ERROR "execution ex_elseif failed")
endif ()
file(READ run_elseif.txt RELIF)
if (NOT RELIF STREQUAL "TWO\n")
    message(FATAL_ERROR "unexpected ex_elseif output: ${RELIF}")
endif ()

# test string comparisons
execute_process(COMMAND ${ILC} front basic -run ${SRC_DIR}/examples/zbasic/ex_str_cmp.bas
        OUTPUT_FILE run_strcmp.txt RESULT_VARIABLE rsc)
if (NOT rsc EQUAL 0)
    message(FATAL_ERROR "execution ex_str_cmp failed")
endif ()
file(READ run_strcmp.txt RSC)
if (NOT RSC STREQUAL "1\n2\n")
    message(FATAL_ERROR "unexpected ex_str_cmp output: ${RSC}")
endif ()

# test float arithmetic and printing
set(tmp_float_bas "${CMAKE_BINARY_DIR}/float_print.bas")
file(WRITE ${tmp_float_bas} "10 LET X# = 1 + 2.5\n20 PRINT X#\n")
execute_process(COMMAND ${ILC} front basic -run ${tmp_float_bas}
        OUTPUT_FILE run_float.txt RESULT_VARIABLE rf)
if (NOT rf EQUAL 0)
    message(FATAL_ERROR "execution float failed")
endif ()
file(READ run_float.txt RF)
if (NOT RF STREQUAL "3.5\n")
    message(FATAL_ERROR "unexpected float output: ${RF}")
endif ()

# test string intrinsics edge cases
set(tmp_str_bas "${CMAKE_BINARY_DIR}/string_intrinsics.bas")
file(WRITE ${tmp_str_bas} "10 PRINT MID$(\"HELLO\",0,2)\n"
        "20 PRINT MID$(\"HELLO\",1,2)\n"
        "30 PRINT LEN(MID$(\"HELLO\",6,2))\n"
        "40 PRINT LEN(MID$(\"HELLO\",1,-1))\n"
        "50 PRINT MID$(\"HELLO\",1,99)\n"
        "60 PRINT LEN(LEFT$(\"HELLO\",0))\n"
        "70 PRINT LEN(RIGHT$(\"HELLO\",0))\n"
        "80 PRINT LEFT$(\"HELLO\",99)\n"
        "90 PRINT RIGHT$(\"HELLO\",99)\n")
execute_process(COMMAND ${ILC} front basic -run ${tmp_str_bas}
        OUTPUT_FILE run_str_intr.txt RESULT_VARIABLE rsi)
if (NOT rsi EQUAL 0)
    message(FATAL_ERROR "execution string_intrinsics failed")
endif ()
file(READ run_str_intr.txt RSI)
if (NOT RSI STREQUAL "HE\nHE\n0\n0\nHELLO\n0\n0\nHELLO\nHELLO\n")
    message(FATAL_ERROR "unexpected string_intrinsics output: ${RSI}")
endif ()

# test conversions STR$, VAL, INT
execute_process(COMMAND ${ILC} front basic -run ${SRC_DIR}/examples/zbasic/ex_conversions.bas
        OUTPUT_FILE run_conv.txt RESULT_VARIABLE rconv)
if (NOT rconv EQUAL 0)
    message(FATAL_ERROR "execution conversions failed")
endif ()
file(READ run_conv.txt RCONV)
if (NOT RCONV STREQUAL "42\n1\n-2\n")
    message(FATAL_ERROR "unexpected conversions output: ${RCONV}")
endif ()

# VAL("abc") returns 0 without trapping
set(tmp_val_zero "${CMAKE_BINARY_DIR}/val_zero.bas")
file(WRITE ${tmp_val_zero} "10 PRINT VAL(\"abc\")\n")
execute_process(COMMAND ${ILC} front basic -run ${tmp_val_zero}
        OUTPUT_FILE val_zero.txt RESULT_VARIABLE rval0)
if (NOT rval0 EQUAL 0)
    message(FATAL_ERROR "VAL(\"abc\") execution failed: ${rval0}")
endif ()
file(READ val_zero.txt VZERO)
if (NOT VZERO STREQUAL "0\n")
    message(FATAL_ERROR "unexpected VAL(\"abc\") output: ${VZERO}")
endif ()

# overflow VAL traps with runtime message
set(tmp_val_over "${CMAKE_BINARY_DIR}/val_overflow.bas")
file(WRITE ${tmp_val_over} "10 PRINT VAL(\"1E400\")\n")
execute_process(COMMAND ${ILC} front basic -run ${tmp_val_over}
        RESULT_VARIABLE rval_over ERROR_FILE val_err.txt)
if (rval_over EQUAL 0)
    message(FATAL_ERROR "expected VAL overflow to trap")
endif ()
file(READ val_err.txt VERR)
# Accept various trap formats:
# Standard VM: "Trap @function:block#ip line N: Kind (code=C)" with Overflow or InvalidCast
# Bytecode VM: "Overflow: ..." or "InvalidCast: ..."
string(REGEX MATCH "Trap @main:[a-zA-Z0-9_]+#[0-9]+ line [0-9]+: (Overflow|InvalidCast) \\(code=[0-9]+\\)" _verr1 "${VERR}")
string(REGEX MATCH "(Overflow|InvalidCast):" _verr2 "${VERR}")
if (NOT _verr1 AND NOT _verr2)
    message(FATAL_ERROR "missing Overflow/InvalidCast trap diagnostic: ${VERR}")
endif ()
