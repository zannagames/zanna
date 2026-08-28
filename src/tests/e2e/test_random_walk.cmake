## File: tests/e2e/test_random_walk.cmake
## Purpose: Ensure BASIC random walk example runs deterministically.
## Key invariants: Output equals expected value.
## Ownership/Lifetime: Invoked by CTest.
## Links: docs/internals/codemap.md

# Use a unique filename to avoid collisions when tests run in parallel.
set(OUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/random_walk.out.txt")
execute_process(COMMAND ${ILC} front basic -run ${SRC_DIR}/examples/zbasic/random_walk.bas
        OUTPUT_FILE ${OUT_FILE} RESULT_VARIABLE r)
if (NOT r EQUAL 0)
    message(FATAL_ERROR "execution failed")
endif ()
file(READ ${OUT_FILE} O)
if (NOT O STREQUAL "4\n")
    message(FATAL_ERROR "unexpected output: ${O}")
endif ()
