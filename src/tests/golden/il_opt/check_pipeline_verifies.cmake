# File: tests/golden/il_opt/check_pipeline_verifies.cmake
# Purpose: Run a whole IL optimizer pipeline and require the result to verify.
# Key invariants: Asserts only the exit status, so no golden text can churn;
# il-opt verifies the optimized module and fails when a pass leaves the IL
# unverifiable. Uses a unique output file per test for parallel runs.
# Ownership/Lifetime: Invoked by CTest for IL optimizer pipeline tests.
# Links: src/il/transform/PassManager.cpp, docs/internals/testing.md

if (NOT DEFINED ILC)
    message(FATAL_ERROR "ILC not set")
endif ()
if (NOT DEFINED IL_FILE)
    message(FATAL_ERROR "IL_FILE not set")
endif ()
if (NOT DEFINED PIPELINE)
    set(PIPELINE "O2")
endif ()

get_filename_component(test_name ${IL_FILE} NAME_WE)
set(OUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/${test_name}.${PIPELINE}.il")
execute_process(
        COMMAND ${ILC} il-opt ${IL_FILE} -o ${OUT_FILE} --pipeline ${PIPELINE}
        OUTPUT_VARIABLE opt_out
        ERROR_VARIABLE opt_err
        RESULT_VARIABLE res)
if (NOT res EQUAL 0)
    message(FATAL_ERROR
            "pipeline ${PIPELINE} left ${IL_FILE} unverifiable\n${opt_out}${opt_err}")
endif ()
