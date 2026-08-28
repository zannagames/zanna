## File: tests/e2e/test_monte_carlo_pi.cmake
## Purpose: Check BASIC Monte Carlo Pi example runs and outputs expected result.
## Key invariants: Floating-point output matches expected range/format.
## Ownership/Lifetime: Invoked by CTest.
## Links: docs/internals/codemap.md

# Use a unique filename to avoid collisions when tests run in parallel.
set(OUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/monte_carlo_pi.out.txt")
execute_process(COMMAND ${ILC} front basic -run ${SRC_DIR}/examples/zbasic/monte_carlo_pi.bas
        OUTPUT_FILE ${OUT_FILE} RESULT_VARIABLE r)
if (NOT r EQUAL 0)
    message(FATAL_ERROR "execution failed")
endif ()
execute_process(COMMAND ${FLOAT_OUT} ${OUT_FILE} ${SRC_DIR}/src/tests/e2e/monte_carlo_pi.expect
        RESULT_VARIABLE r2)
if (NOT r2 EQUAL 0)
    message(FATAL_ERROR "float mismatch")
endif ()
