#===----------------------------------------------------------------------===//
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===//
#
# File: src/tests/tools/ProjectTemplateSmoke.cmake
# Purpose: End-to-end game-template smoke — scaffold both templates with
#          the Studio service, then run each generated game's --smoke
#          contract (ADR 0225).
# Key invariants:
#   - Fails loudly on any stage: scaffold, shape/schema verify, or either
#     generated game not printing "RESULT: ok".
# Ownership/Lifetime:
#   - Uses a fresh directory under the build tree; recreated per run.
# Links: src/zannastudio/src/probes/project_template_scaffold_probe.zia
#
#===----------------------------------------------------------------------===//

if (NOT DEFINED ZIA_BIN OR NOT DEFINED SOURCE_DIR OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "ProjectTemplateSmoke requires ZIA_BIN, SOURCE_DIR, WORK_DIR")
endif ()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

execute_process(
        COMMAND "${ZIA_BIN}"
        "${SOURCE_DIR}/src/zannastudio/src/probes/project_template_scaffold_probe.zia"
        -- "${WORK_DIR}"
        WORKING_DIRECTORY "${SOURCE_DIR}/src/zannastudio/src/probes"
        RESULT_VARIABLE _rv
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
if (NOT _rv EQUAL 0 OR NOT _out MATCHES "RESULT: ok")
    message(FATAL_ERROR "template scaffold failed\nstdout:\n${_out}\nstderr:\n${_err}")
endif ()

foreach (_proj starter2d starter3d)
    execute_process(
            COMMAND "${ZIA_BIN}" src/main.zia -- --smoke
            WORKING_DIRECTORY "${WORK_DIR}/${_proj}"
            RESULT_VARIABLE _rv
            OUTPUT_VARIABLE _out
            ERROR_VARIABLE _err)
    if (NOT _rv EQUAL 0 OR NOT _out MATCHES "RESULT: ok")
        message(FATAL_ERROR "${_proj} --smoke failed\nstdout:\n${_out}\nstderr:\n${_err}")
    endif ()
endforeach ()

message(STATUS "project template smoke passed")
