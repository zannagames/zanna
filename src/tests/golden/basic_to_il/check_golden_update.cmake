# Validate that every BASIC-to-IL golden harness can refresh its own goldens.
#
# Purpose: A runtime function that becomes always-declared changes only the
#          extern block of every BASIC golden. Both harnesses used to treat that
#          as an unrecoverable mismatch, so scripts/update_goldens.sh could not
#          repair the tree and the files had to be edited by hand. This test
#          drives the seed, detect, and refresh cycle for check_il.cmake,
#          check_il_bounds.cmake, and the compiled batch runner.
#
# Inputs: ILC, BATCH_RUNNER, CHECK_IL, CHECK_IL_BOUNDS, BAS_FILE, WORK_DIR.
#
# Every golden written here lives under WORK_DIR; no checked-in golden is read
# or modified.

foreach (_required ILC BATCH_RUNNER CHECK_IL CHECK_IL_BOUNDS BAS_FILE WORK_DIR)
    if (NOT DEFINED ${_required})
        message(FATAL_ERROR "${_required} not set")
    endif ()
endforeach ()

file(REMOVE_RECURSE ${WORK_DIR})
file(MAKE_DIRECTORY ${WORK_DIR})

## Run one command and assert its exit status.
## UPDATE_GOLDEN is forced empty so an inherited value (update_goldens.sh
## exports one) can never turn an expected failure into a silent rewrite.
## Stores combined output in _last_output for the callers that inspect it.
function(_expect_status label expect_success)
    execute_process(
            COMMAND ${CMAKE_COMMAND} -E env UPDATE_GOLDEN= ${ARGN}
            RESULT_VARIABLE _res
            OUTPUT_VARIABLE _out
            ERROR_VARIABLE _err)
    if (expect_success AND NOT _res EQUAL 0)
        message(FATAL_ERROR "${label}: expected success, got exit ${_res}\n${_out}\n${_err}")
    endif ()
    if (NOT expect_success AND _res EQUAL 0)
        message(FATAL_ERROR "${label}: expected failure, got success\n${_out}\n${_err}")
    endif ()
    set(_last_output "${_out}${_err}" PARENT_SCOPE)
endfunction()

## Run a command with UPDATE_GOLDEN=1 in the environment and require success.
## This is the route scripts/update_goldens.sh uses for every harness, so both
## the cmake scripts and the compiled runner are exercised through it.
function(_expect_env_update label)
    execute_process(
            COMMAND ${CMAKE_COMMAND} -E env UPDATE_GOLDEN=1 ${ARGN}
            RESULT_VARIABLE _res
            OUTPUT_VARIABLE _out
            ERROR_VARIABLE _err)
    if (NOT _res EQUAL 0)
        message(FATAL_ERROR "${label}: expected success, got exit ${_res}\n${_out}\n${_err}")
    endif ()
endfunction()

## Run a command with UPDATE_GOLDEN=0 and require failure: an explicit "off"
## value must never be mistaken for a refresh request.
function(_expect_disabled label)
    execute_process(
            COMMAND ${CMAKE_COMMAND} -E env UPDATE_GOLDEN=0 ${ARGN}
            RESULT_VARIABLE _res
            OUTPUT_VARIABLE _out
            ERROR_VARIABLE _err)
    if (_res EQUAL 0)
        message(FATAL_ERROR "${label}: UPDATE_GOLDEN=0 refreshed the golden\n${_out}\n${_err}")
    endif ()
endfunction()

## Delete exactly one extern declaration from a golden file.
## This is the drift a newly always-declared runtime function produces: the IL
## body is untouched and only the extern set differs.
function(_drop_one_extern golden)
    file(READ ${golden} _text)
    string(REGEX MATCH "extern @[^\n]+\n" _extern "${_text}")
    if ("${_extern}" STREQUAL "")
        message(FATAL_ERROR "no extern declaration found in ${golden}")
    endif ()
    string(REPLACE "${_extern}" "" _text "${_text}")
    file(WRITE ${golden} "${_text}")
endfunction()

## Fail unless text contains a substring.
function(_expect_contains label text needle)
    string(FIND "${text}" "${needle}" _pos)
    if (_pos EQUAL -1)
        message(FATAL_ERROR "${label}: expected output to contain '${needle}'\n${text}")
    endif ()
endfunction()

# ---------------------------------------------------------------------------
# check_il.cmake
# ---------------------------------------------------------------------------
set(_compare_golden ${WORK_DIR}/compare.il)
set(_compare_cmd ${CMAKE_COMMAND} -DILC=${ILC} -DBAS_FILE=${BAS_FILE}
        -DGOLDEN=${_compare_golden} -P ${CHECK_IL})
set(_compare_update_cmd ${CMAKE_COMMAND} -DILC=${ILC} -DBAS_FILE=${BAS_FILE}
        -DGOLDEN=${_compare_golden} -DUPDATE_GOLDEN=1 -P ${CHECK_IL})

file(WRITE ${_compare_golden} "")
_expect_status("check_il empty golden" FALSE ${_compare_cmd})
_expect_status("check_il seed" TRUE ${_compare_update_cmd})
_expect_status("check_il after seed" TRUE ${_compare_cmd})

# A refreshed golden must hold verbatim compiler output, not the normalized
# comparison form: goldens carry the real IL version header.
file(READ ${_compare_golden} _seeded)
if (NOT "${_seeded}" MATCHES "^il [0-9]+\\.[0-9]+\\.[0-9]+\n")
    message(FATAL_ERROR "check_il seed wrote a normalized golden instead of raw output:\n${_seeded}")
endif ()

_drop_one_extern(${_compare_golden})
_expect_status("check_il extern drift" FALSE ${_compare_cmd})
_expect_contains("check_il extern drift" "${_last_output}" "Extern declarations mismatch")
_expect_disabled("check_il extern drift with UPDATE_GOLDEN=0" ${_compare_cmd})
# Refresh through the environment, which is the route update_goldens.sh takes.
_expect_env_update("check_il extern refresh via environment" ${_compare_cmd})
_expect_status("check_il after extern refresh" TRUE ${_compare_cmd})

# ---------------------------------------------------------------------------
# check_il_bounds.cmake
# ---------------------------------------------------------------------------
set(_bounds_golden ${WORK_DIR}/bounds.il)
set(_bounds_cmd ${CMAKE_COMMAND} -DILC=${ILC} -DBAS_FILE=${BAS_FILE}
        -DGOLDEN=${_bounds_golden} -P ${CHECK_IL_BOUNDS})
set(_bounds_update_cmd ${CMAKE_COMMAND} -DILC=${ILC} -DBAS_FILE=${BAS_FILE}
        -DGOLDEN=${_bounds_golden} -DUPDATE_GOLDEN=1 -P ${CHECK_IL_BOUNDS})

file(WRITE ${_bounds_golden} "")
_expect_status("check_il_bounds empty golden" FALSE ${_bounds_cmd})
_expect_env_update("check_il_bounds seed via environment" ${_bounds_cmd})
_expect_status("check_il_bounds after seed" TRUE ${_bounds_cmd})

_drop_one_extern(${_bounds_golden})
_expect_status("check_il_bounds extern drift" FALSE ${_bounds_cmd})
_expect_status("check_il_bounds extern refresh via -D" TRUE ${_bounds_update_cmd})
_expect_status("check_il_bounds after extern refresh" TRUE ${_bounds_cmd})

# ---------------------------------------------------------------------------
# BasicToIlBatchRunner
# ---------------------------------------------------------------------------
set(_batch_compare_golden ${WORK_DIR}/batch_compare.il)
set(_batch_bounds_golden ${WORK_DIR}/batch_bounds.il)
set(_manifest ${WORK_DIR}/cases.tsv)
file(WRITE ${_manifest}
        "compare\tupdate_compare\t${BAS_FILE}\t${_batch_compare_golden}\t\t\n"
        "bounds\tupdate_bounds\t${BAS_FILE}\t${_batch_bounds_golden}\t\t\n")

file(WRITE ${_batch_compare_golden} "")
file(WRITE ${_batch_bounds_golden} "")
_expect_status("batch empty goldens" FALSE ${BATCH_RUNNER} ${_manifest})
_expect_env_update("batch seed" ${BATCH_RUNNER} ${_manifest})
_expect_status("batch after seed" TRUE ${BATCH_RUNNER} ${_manifest})

_drop_one_extern(${_batch_compare_golden})
_expect_status("batch extern drift" FALSE ${BATCH_RUNNER} ${_manifest})
_expect_contains("batch extern drift" "${_last_output}" "extern declarations mismatch")
_expect_disabled("batch extern drift with UPDATE_GOLDEN=0" ${BATCH_RUNNER} ${_manifest})
_expect_env_update("batch extern refresh" ${BATCH_RUNNER} ${_manifest})
_expect_status("batch after extern refresh" TRUE ${BATCH_RUNNER} ${_manifest})

file(REMOVE_RECURSE ${WORK_DIR})
message(STATUS "golden update path verified for check_il, check_il_bounds, and the batch runner")
