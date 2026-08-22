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
get_filename_component(test_name ${IL_FILE} NAME_WE)
set(OUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/${test_name}.constfold.il")
execute_process(
        COMMAND ${ILC} il-opt ${IL_FILE} -o ${OUT_FILE} --passes constfold
        RESULT_VARIABLE opt_res
        OUTPUT_VARIABLE opt_out
        ERROR_VARIABLE opt_err)
if (NOT opt_res EQUAL 0)
    message(FATAL_ERROR "il-opt failed: ${opt_res}\n${opt_out}\n${opt_err}")
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
                RESULT_VARIABLE diff_res
                OUTPUT_VARIABLE diff_out)
        set(art_dir "${CMAKE_CURRENT_BINARY_DIR}/_artifacts/constfold_${test_name}")
        file(MAKE_DIRECTORY ${art_dir})
        configure_file(${GOLDEN} ${art_dir}/golden.il COPYONLY)
        configure_file(${OUT_FILE} ${art_dir}/out.il COPYONLY)
        file(WRITE ${art_dir}/diff.txt "${diff_out}")
        message(FATAL_ERROR "IL mismatch for constfold_${test_name}:\n${diff_out}")
    endif ()
endif ()
if (NOT DEFINED SKIP_RUNTIME)
    execute_process(
            COMMAND ${ILC} -run ${IL_FILE}
            RESULT_VARIABLE base_res
            OUTPUT_VARIABLE base_out
            ERROR_VARIABLE base_err)
    execute_process(
            COMMAND ${ILC} -run ${OUT_FILE}
            RESULT_VARIABLE folded_res
            OUTPUT_VARIABLE folded_out
            ERROR_VARIABLE folded_err)
    # Normalize Windows line endings for comparison
    string(REPLACE "\r\n" "\n" base_out "${base_out}")
    string(REPLACE "\r" "\n" base_out "${base_out}")
    string(REPLACE "\r\n" "\n" base_err "${base_err}")
    string(REPLACE "\r" "\n" base_err "${base_err}")
    string(REPLACE "\r\n" "\n" folded_out "${folded_out}")
    string(REPLACE "\r" "\n" folded_out "${folded_out}")
    string(REPLACE "\r\n" "\n" folded_err "${folded_err}")
    string(REPLACE "\r" "\n" folded_err "${folded_err}")
    if (NOT base_res EQUAL folded_res)
        message(FATAL_ERROR "exit status mismatch: ${base_res} vs ${folded_res}")
    endif ()
    if (NOT base_out STREQUAL folded_out)
        message(FATAL_ERROR "stdout mismatch\n--- base ---\n${base_out}\n--- folded ---\n${folded_out}")
    endif ()
    if (NOT base_err STREQUAL folded_err)
        message(FATAL_ERROR "stderr mismatch\n--- base ---\n${base_err}\n--- folded ---\n${folded_err}")
    endif ()
    if (DEFINED EXPECT_RESULT)
        if (NOT base_res EQUAL EXPECT_RESULT)
            message(FATAL_ERROR "expected exit ${EXPECT_RESULT} but got ${base_res}")
        endif ()
    elseif (DEFINED EXPECT_NONZERO)
        if (base_res EQUAL 0)
            message(FATAL_ERROR "expected non-zero exit")
        endif ()
    endif ()
    if (DEFINED EXPECT_REGEX)
        if (NOT base_out MATCHES "${EXPECT_REGEX}" AND NOT base_err MATCHES "${EXPECT_REGEX}")
            message(FATAL_ERROR "expected regex '${EXPECT_REGEX}' not found in output")
        endif ()
    endif ()
endif ()
