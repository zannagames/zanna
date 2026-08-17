#===----------------------------------------------------------------------===#
#
# Part of the Zanna project, under the GNU GPL v3.
# See LICENSE for license information.
#
#===----------------------------------------------------------------------===#
#
# File: src/tests/tools/ZannaStudioLinkedWorkspaceTests.cmake
# Purpose: Run the Studio linked-descendant probe against a real filesystem link.
# Key invariants:
#   - The linked directory targets a sibling outside the opened workspace.
#   - Unsupported link creation skips cleanly without hiding probe failures.
# Ownership/Lifetime: Owns and removes TEST_WORK_DIR and every fixture below it.
# Cross-platform touchpoints: CMake creates the native symbolic link; platforms
#                             that deny link creation report a clean skip.
# Links: docs/adr/0153-non-following-path-link-inspection.md
#        docs/adr/0259-stable-filesystem-entry-identity.md
#
#===----------------------------------------------------------------------===#

foreach(_required IN ITEMS ZIA_EXECUTABLE PROBE_SOURCE TEST_WORK_DIR)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "${_required} is required")
  endif()
endforeach()
if(NOT EXISTS "${ZIA_EXECUTABLE}" OR NOT EXISTS "${PROBE_SOURCE}")
  message(FATAL_ERROR "linked-workspace probe inputs are missing")
endif()

set(_workspace "${TEST_WORK_DIR}/workspace")
set(_outside "${TEST_WORK_DIR}/outside")
set(_linked_dir "${_workspace}/linked-outside")
set(_linked_file "${_linked_dir}/outside.zia")

file(REMOVE_RECURSE "${TEST_WORK_DIR}")
file(MAKE_DIRECTORY "${_workspace}" "${_outside}")
file(WRITE "${_outside}/outside.zia" "module OutsideLinkedWorkspace;\n")
file(CREATE_LINK "${_outside}" "${_linked_dir}" SYMBOLIC RESULT _link_result)
if(NOT "${_link_result}" STREQUAL "0")
  file(REMOVE_RECURSE "${TEST_WORK_DIR}")
  message("RESULT: ok (symbolic-link fixture unavailable: ${_link_result})")
  return()
endif()

execute_process(
  COMMAND "${ZIA_EXECUTABLE}" "${PROBE_SOURCE}" --
          "${_workspace}" "${_linked_dir}" "${_linked_file}" "${_outside}"
  RESULT_VARIABLE _probe_result
  OUTPUT_VARIABLE _probe_stdout
  ERROR_VARIABLE _probe_stderr
  TIMEOUT 30)

# Remove the link explicitly before recursively cleaning its parent fixture.
file(REMOVE "${_linked_dir}")
file(REMOVE_RECURSE "${TEST_WORK_DIR}")

if(NOT _probe_result EQUAL 0 OR NOT _probe_stdout MATCHES "RESULT: ok")
  message(FATAL_ERROR
          "linked-workspace probe failed (${_probe_result})\n"
          "stdout:\n${_probe_stdout}\nstderr:\n${_probe_stderr}")
endif()
message("${_probe_stdout}")
