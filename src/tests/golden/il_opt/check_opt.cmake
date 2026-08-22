# File: tests/golden/il_opt/check_opt.cmake
# Purpose: Run the IL optimizer and compare its output to a golden file.
# Key invariants: Uses a unique output file per test to prevent cross-test
# clobbering during parallel runs. Emits helpful diffs and artifacts on failure.
# Ownership/Lifetime: Invoked by CTest for IL optimizer golden tests.
# Links: docs/internals/codemap.md

## Accept UPDATE_GOLDEN from -D or the environment.
include(${CMAKE_CURRENT_LIST_DIR}/../GoldenUpdateMode.cmake)

if (NOT DEFINED ILC)
    message(FATAL_ERROR "ILC not set")
endif ()
if (NOT DEFINED IL_FILE)
    message(FATAL_ERROR "IL_FILE not set")
endif ()
if (NOT DEFINED GOLDEN)
    message(FATAL_ERROR "GOLDEN not set")
endif ()
if (NOT DEFINED PASSES)
    set(PASSES "constfold,peephole")
endif ()
get_filename_component(test_name ${IL_FILE} NAME_WE)
set(OUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/${test_name}.out.il")
execute_process(
        COMMAND ${ILC} il-opt ${IL_FILE} -o ${OUT_FILE} --passes ${PASSES}
        RESULT_VARIABLE res)
if (NOT res EQUAL 0)
    message(FATAL_ERROR "il-opt failed")
endif ()
## Normalize IL version to avoid test churn on version bumps.
file(READ ${GOLDEN} golden_content)
file(READ ${OUT_FILE} out_content)
# Normalize Windows line endings
string(REPLACE "\r\n" "\n" golden_content "${golden_content}")
string(REPLACE "\r" "\n" golden_content "${golden_content}")
string(REPLACE "\r\n" "\n" out_content "${out_content}")
string(REPLACE "\r" "\n" out_content "${out_content}")
string(REGEX REPLACE "^il [0-9]+\\.[0-9]+\\.[0-9]+" "il VERSION" golden_content "${golden_content}")
string(REGEX REPLACE "^il [0-9]+\\.[0-9]+\\.[0-9]+" "il VERSION" out_content "${out_content}")
if (NOT out_content STREQUAL golden_content)
    if (DEFINED UPDATE_GOLDEN)
        file(READ ${OUT_FILE} _raw)
        file(WRITE ${GOLDEN} "${_raw}")
        message(STATUS "Updated golden: ${GOLDEN}")
    else ()
        execute_process(
                COMMAND diff -u ${GOLDEN} ${OUT_FILE}
                OUTPUT_VARIABLE diff
                RESULT_VARIABLE diff_res)
        set(art_dir "${CMAKE_CURRENT_BINARY_DIR}/_artifacts/il_opt_${test_name}")
        file(MAKE_DIRECTORY ${art_dir})
        configure_file(${GOLDEN} ${art_dir}/golden.il COPYONLY)
        configure_file(${OUT_FILE} ${art_dir}/out.il COPYONLY)
        file(WRITE ${art_dir}/diff.txt "${diff}")
        execute_process(COMMAND ${CMAKE_COMMAND} -E echo
                "Expected (first 50 lines):")
        execute_process(COMMAND sh -c "cat -n ${GOLDEN} | head -n 50")
        execute_process(COMMAND ${CMAKE_COMMAND} -E echo
                "Got (first 50 lines):")
        execute_process(COMMAND sh -c "cat -n ${OUT_FILE} | head -n 50")
        execute_process(COMMAND ${CMAKE_COMMAND} -E echo "Diff:\n${diff}")
        message(FATAL_ERROR "IL mismatch")
    endif ()
endif ()
