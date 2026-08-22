# SPDX-License-Identifier: GPL-3.0-only
# File: tests/golden/arrays/check_basic_run_output.cmake
# Purpose: Run BASIC frontend on a program and check stdout.
## Accept UPDATE_GOLDEN from -D or the environment.
include(${CMAKE_CURRENT_LIST_DIR}/../GoldenUpdateMode.cmake)

if (NOT DEFINED ILC)
    message(FATAL_ERROR "ILC not set")
endif ()
if (NOT DEFINED BAS_FILE)
    message(FATAL_ERROR "BAS_FILE not set")
endif ()
if (NOT DEFINED EXPECT)
    message(FATAL_ERROR "EXPECT not set")
endif ()
# Optional pre-test cleanup for files created by previous test runs
if (DEFINED CLEANUP_FILE)
    file(REMOVE "${CLEANUP_FILE}")
endif ()
# DEBUG_VM: when set, use standard VM (--debug-vm) for tests requiring runtime exception handling
if (DEFINED DEBUG_VM)
    execute_process(
            COMMAND ${ILC} front basic --debug-vm -run ${BAS_FILE}
            RESULT_VARIABLE res
            OUTPUT_VARIABLE out
            ERROR_VARIABLE err)
else ()
    execute_process(
            COMMAND ${ILC} front basic -run ${BAS_FILE}
            RESULT_VARIABLE res
            OUTPUT_VARIABLE out
            ERROR_VARIABLE err)
endif ()
if (NOT res EQUAL 0)
    message(FATAL_ERROR "expected zero exit: ${res} stderr: ${err}")
endif ()
string(REPLACE "\r\n" "\n" out "${out}")
string(REGEX REPLACE "\n+$" "" out "${out}")
if (NOT out STREQUAL "${EXPECT}")
    if (DEFINED UPDATE_GOLDEN AND DEFINED GOLDEN_FILE)
        file(WRITE ${GOLDEN_FILE} "${out}\n")
        message(STATUS "Updated golden: ${GOLDEN_FILE}")
    else ()
        message(FATAL_ERROR "output mismatch: expected '${EXPECT}' got '${out}'")
    endif ()
endif ()
